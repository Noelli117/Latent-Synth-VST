#pragma once
#include <JuceHeader.h>
#include <mutex>
#include <torch/script.h>
#include <torch/torch.h>

#define MAX_LATENT_BUFFER_SIZE 32
#define BUFFER_LENGTH 32768
using namespace torch::indexing;

class RAVE : public juce::ChangeBroadcaster {

public:
  RAVE() : juce::ChangeBroadcaster() {
    torch::jit::getProfilingMode() = false;
    c10::InferenceMode guard;
    torch::jit::setGraphExecutorOptimize(true);
  }

  void load_model(const std::string &rave_model_file) {
    torch::jit::Module loadedModel;
    try {
      c10::InferenceMode guard;
      loadedModel = torch::jit::load(rave_model_file);
    } catch (const c10::Error &e) {
      std::cerr << e.what();
      std::cerr << e.msg();
      std::cerr << "error loading the model\n";
      return;
    }

    auto named_buffers = loadedModel.named_buffers();
    auto named_attributes = loadedModel.named_attributes();
    bool loadedHasPrior = false;
    bool loadedStereo = false;
    int loadedSr = 0;
    int loadedLatentSize = 0;
    at::Tensor loadedEncodeParams;
    at::Tensor loadedDecodeParams;
    at::Tensor loadedPriorParams = torch::zeros({0});

    std::cout << "[ ] RAVE - Model successfully loaded: " << rave_model_file
              << std::endl;

    bool found_model_as_attribute = false;
    bool found_stereo_attribute = false;
    for (auto const& attr : named_attributes) {
      if (attr.name == "_rave") {
        found_model_as_attribute = true;
        std::cout << "Found _rave model as named attribute" << std::endl;
      }
      else if (attr.name == "stereo" || attr.name == "_rave.stereo") {
        found_stereo_attribute = true;
        loadedStereo = attr.value.toBool();
        std::cout << "Stereo?" << (loadedStereo ? "true" : "false") << std::endl;
      }
    }

    if (found_model_as_attribute) {
      // Use named buffers within _rave
      for (auto const& buf : named_buffers) {
        if (buf.name == "_rave.sampling_rate") {
          loadedSr = buf.value.item<int>();
          std::cout << "\tSampling rate: " << loadedSr << std::endl;
        }
        else if (buf.name == "_rave.latent_size") {
          loadedLatentSize = buf.value.item<int>();
          std::cout << "\tLatent size: " << loadedLatentSize << std::endl;
        }
        else if (buf.name == "encode_params") {
          loadedEncodeParams = buf.value;
          std::cout << "\tEncode parameters: " << loadedEncodeParams << std::endl;
        }
        else if (buf.name == "decode_params") {
          loadedDecodeParams = buf.value;
          std::cout << "\tDecode parameters: " << loadedDecodeParams << std::endl;
        }
        else if (buf.name == "prior_params") {
          loadedPriorParams = buf.value;
          loadedHasPrior = true;
          std::cout << "\tPrior parameters: " << loadedPriorParams << std::endl;
        }
      }
    }
    else {
      // Use top-level named attributes
      for (auto const& attr : named_attributes) {
        if (attr.name == "sampling_rate") {
          loadedSr = static_cast<int>(attr.value.toInt());
          std::cout << "\tSampling rate: " << loadedSr << std::endl;
        }
        else if (attr.name == "full_latent_size") {
          loadedLatentSize = static_cast<int>(attr.value.toInt());
          std::cout << "\tLatent size: " << loadedLatentSize << std::endl;
        }
        else if (attr.name == "encode_params") {
          loadedEncodeParams = attr.value.toTensor();
          std::cout << "\tEncode parameters: " << loadedEncodeParams << std::endl;
        }
        else if (attr.name == "decode_params") {
          loadedDecodeParams = attr.value.toTensor();
          std::cout << "\tDecode parameters: " << loadedDecodeParams << std::endl;
        }
        else if (attr.name == "prior_params") {
          loadedPriorParams = attr.value.toTensor();
          loadedHasPrior = true;
          std::cout << "\tPrior parameters: " << loadedPriorParams << std::endl;
        }
      }
    }

    const std::lock_guard<std::mutex> lock(modelMutex);
    model = std::move(loadedModel);
    model_path = juce::String(rave_model_file);
    sr = loadedSr;
    latent_size = loadedLatentSize;
    has_prior = loadedHasPrior;
    stereo = found_stereo_attribute ? loadedStereo : false;
    encode_params = loadedEncodeParams;
    decode_params = loadedDecodeParams;
    prior_params = loadedPriorParams;

    std::cout << "\tFull latent size: " << latent_size << std::endl;
    std::cout << "\tRatio: " << encode_params.index({3}).item<int>() << std::endl;
    sendChangeMessage();
  }

  torch::Tensor sample_prior(const int n_steps, const float temperature) {
    c10::InferenceMode guard;
    const std::lock_guard<std::mutex> lock(modelMutex);
    std::vector<torch::jit::IValue> inputs_rave;
    inputs_rave.push_back(torch::ones({1, 1, n_steps}) * temperature);
    torch::Tensor prior = this->model.get_method("prior")(inputs_rave).toTensor();
    return prior;
  }

  torch::Tensor encode(const torch::Tensor input) {
    c10::InferenceMode guard;
    const std::lock_guard<std::mutex> lock(modelMutex);
    std::vector<torch::jit::IValue> inputs_rave;
    inputs_rave.push_back(input);
    auto y = this->model.get_method("encode")(inputs_rave).toTensor();
    return y;
  }

  std::vector<torch::Tensor> encode_amortized(const torch::Tensor input) {
    c10::InferenceMode guard;
    const std::lock_guard<std::mutex> lock(modelMutex);
    std::vector<torch::jit::IValue> inputs_rave;
    inputs_rave.push_back(input);
    auto stats = this->model.get_method("encode_amortized")(inputs_rave)
                     .toTuple()
                     .get()
                     ->elements();
    torch::Tensor mean = stats[0].toTensor();
    torch::Tensor std = stats[1].toTensor();
    std::vector<torch::Tensor> mean_std = {mean, std};
    return mean_std;
  }

  torch::Tensor decode(const torch::Tensor input) {
    c10::InferenceMode guard;
    const std::lock_guard<std::mutex> lock(modelMutex);
    std::vector<torch::jit::IValue> inputs_rave;
    inputs_rave.push_back(input);
    auto y = this->model.get_method("decode")(inputs_rave).toTensor();
    return y;
  }

  juce::Range<float> getValidBufferSizes() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return juce::Range<float>(encode_params.index({3}).item<int>(), BUFFER_LENGTH);
  }

  unsigned int getLatentDimensions() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    int tmp = decode_params.index({0}).item<int>();
    assert(tmp >= 0);
    return (unsigned int)tmp;
  }

  unsigned int getEncodeChannels() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    int tmp = encode_params.index({0}).item<int>();
    assert(tmp >= 0);
    return (unsigned int)tmp;
  }

  unsigned int getDecodeChannels() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    int tmp = decode_params.index({3}).item<int>();
    assert(tmp >= 0);
    return (unsigned int)tmp;
  }

  int getModelRatio() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return encode_params.index({3}).item<int>();
  }

  float zPerSeconds() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return encode_params.index({3}).item<float>() / sr;
  }

  int getFullLatentDimensions() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return latent_size;
  }

  int getInputBatches() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return encode_params.index({1}).item<int>();
  }

  int getOutputBatches() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return decode_params.index({3}).item<int>();
  }

  void resetLatentBuffer() { latent_buffer = torch::zeros({0}); }

  void writeLatentBuffer(at::Tensor latent) {
    const std::lock_guard<std::mutex> lock(modelMutex);
    if (latent_buffer.size(0) == 0) {
      latent_buffer = latent;
    } else {
      latent_buffer = torch::cat({latent_buffer, latent}, 2);
    }
    if (latent_buffer.size(2) > MAX_LATENT_BUFFER_SIZE) {
      latent_buffer = latent_buffer.index(
          {"...", Slice(-MAX_LATENT_BUFFER_SIZE, None, None)});
    }
  }

  bool hasPrior() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return has_prior;
  }

  bool isStereo() const {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return stereo;
  }

  at::Tensor getLatentBuffer() {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return latent_buffer;
  }

  bool hasMethod(const std::string& method_name) const {
    const std::lock_guard<std::mutex> lock(modelMutex);
    return this->model.find_method(method_name).has_value();
  }

private:
  mutable std::mutex modelMutex;
  torch::jit::Module model;
  int sr = 0;
  int latent_size = 0;
  bool has_prior = false;
  bool stereo = false;
  juce::String model_path;
  at::Tensor encode_params;
  at::Tensor decode_params;
  at::Tensor prior_params;
  at::Tensor latent_buffer;
  juce::Range<float> validBufferSizeRange;
};

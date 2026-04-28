#include "PluginEditor.h"
#include "PluginProcessor.h"
#include <algorithm>
#include <math.h>

#define DEBUG_PERFORM 0

namespace {
constexpr float kInputRmsMuteThreshold = 0.000316f; // roughly -70 dBFS
}

void RaveAP::modelPerform() {
  auto *rave = _rave.get();
  if (rave != nullptr && !_isMuted.load()) {
    try {
    c10::InferenceMode guard(true);
    // encode
    int input_size = static_cast<int>(pow(2, *_latencyMode));

    const bool useExternalLatent = _externalLatentMode.load();
    if (!useExternalLatent && !static_cast<bool>(*_usePrior)) {
      double sumSquares = 0.0;
      const auto frameSize = static_cast<size_t>(input_size);
      for (size_t i = 0; i < frameSize; ++i) {
        const float sample = _inModel[0][i];
        sumSquares += static_cast<double>(sample) * sample;
      }

      const double inputMeanSquare = sumSquares / std::max(1, input_size);
      const double muteMeanSquare =
          static_cast<double>(kInputRmsMuteThreshold) *
          kInputRmsMuteThreshold;
      if (inputMeanSquare < muteMeanSquare) {
        std::fill(_outModel[0].get(), _outModel[0].get() + input_size, 0.0f);
        std::fill(_outModel[1].get(), _outModel[1].get() + input_size, 0.0f);
        return;
      }
    }

    at::Tensor latent_traj;
    at::Tensor latent_traj_mean;

#if DEBUG_PERFORM
    std::cout << "exp: " << *_latencyMode << " value: " << input_size << '\n';
    std::cout << "has prior : " << _rave->hasPrior()
              << "; use prior : " << *_usePrior << std::endl;
    std::cout << "temperature : " << *_priorTemperature << std::endl;
#endif

    if (rave->hasPrior() && *_usePrior) {
      auto n_trajs = pow(2, *_latencyMode) / rave->getModelRatio();
      latent_traj = rave->sample_prior((int)n_trajs, *_priorTemperature);
      latent_traj_mean = latent_traj;
    } else {
      int64_t sizes = {input_size};
      at::Tensor frame = torch::from_blob(_inModel[0].get(), sizes);
      frame = torch::reshape(frame, {1, 1, input_size});
#if DEBUG_PERFORM
      std::cout << "Current input size : " << frame.sizes() << std::endl;
#endif DEBUG_PERFORM

      if (rave->hasMethod("encode_amortized")) {
        std::vector<torch::Tensor> latent_probs = rave->encode_amortized(frame);
        latent_traj_mean = latent_probs[0];
        at::Tensor latent_traj_std = latent_probs[1];

  #if DEBUG_PERFORM
        std::cout << "mean shape" << latent_traj_mean.sizes() << std::endl;
        std::cout << "std shape" << latent_traj_std.sizes() << std::endl;
  #endif

        latent_traj = latent_traj_mean +
                      latent_traj_std * torch::randn_like(latent_traj_mean);
      } else {
        latent_traj = rave->encode(frame);
        latent_traj_mean = latent_traj;
      }
    }

#if DEBUG_PERFORM
    std::cout << "latent traj shape" << latent_traj.sizes() << std::endl;
#endif

    // Latent modifications
    if (useExternalLatent) {
      LatentFlowController::Params flowParams;
      flowParams.speed = _webUiFlowSpeed.load();
      flowParams.noiseScale = _webUiFlowNoiseScale.load();
      flowParams.curve = _webUiFlowCurve.load();
      flowParams.gain = _webUiFlowGain.load();
      flowParams.contrast = _webUiFlowContrast.load();
      flowParams.intensity = _webUiFlowIntensity.load();
      _latentFlowController.setParams(flowParams);
      const float stepDt =
          (_sampleRate > 0.0) ? ((float)input_size / (float)_sampleRate)
                              : (1.0f / 60.0f);
      _latentFlowController.step(stepDt);
      const auto &flowLatent = _latentFlowController.getLatent();
      updateRuntimeExternalLatentValues(flowLatent);
      _webUiFlowRadius.store(_latentFlowController.getRadius());
    }
    int64_t n_dimensions =
        std::min((int)latent_traj.size(1), (int)AVAILABLE_DIMS);

    if (useExternalLatent) {
      for (int i = 0; i < n_dimensions; i++) {
        assert(i >= 0);
        auto i2 = (long unsigned int)i;
        float externalValue = _externalLatentValues[i2].load();
        latent_traj.index_put_({0, i}, externalValue);
        latent_traj_mean.index_put_({0, i}, externalValue);
      }
    } else {
      // apply scale and bias
      for (int i = 0; i < n_dimensions; i++) {
        // The assert and casting here is needed as I got a:
        // warning: conversion to ‘std::array<std::atomic<float>*,
        // 8>::size_type’ {aka ‘long unsigned int’} from ‘int’ may change the
        // sign of the result [-Wsign-conversion]
        // Whatever AVAILABLE_DIMS type I defined
        assert(i >= 0);
        auto i2 = (long unsigned int)i;
        float scale = _latentScale[i2]->load();
        float bias = _latentBias[i2]->load();
        latent_traj.index_put_({0, i},
                               (latent_traj.index({0, i}) * scale + bias));
        latent_traj_mean.index_put_(
            {0, i}, (latent_traj_mean.index({0, i}) * scale + bias));
      }
    }
    // Safety-first: latent buffer writes can throw on shape mismatches and
    // bring down the host. Keep audio running and skip this update if invalid.
    try {
      rave->writeLatentBuffer(latent_traj_mean);
    } catch (const c10::Error &) {
    }

#if DEBUG_PERFORM
    std::cout << "scale & bias applied" << std::endl;
#endif

    // Keep native latent jitter only in native mode.
    // In external/global latent mode, jitter is owned by the web controller.
    float jitter_amount = useExternalLatent ? 0.0f : _latentJitterValue->load();
    latent_traj = latent_traj + jitter_amount * torch::randn_like(latent_traj);

#if DEBUG_PERFORM
    std::cout << "jitter applied" << std::endl;
#endif

    // filling missing dimensions with width parameter
    int missing_dims = rave->getFullLatentDimensions() - latent_traj.size(1);

    if (rave->isStereo() && missing_dims > 0) {
      torch::Tensor latent_trajL = latent_traj,
                    latent_trajR = latent_traj.clone();
      int missing_dims = rave->getFullLatentDimensions() - latent_trajL.size(1);
      float width = _widthValue->load() / 100.f;
      at::Tensor latent_noiseL =
          torch::randn({1, missing_dims, latent_trajL.size(2)});
      at::Tensor latent_noiseR =
          (1 - width) * latent_noiseL +
          width * torch::randn({1, missing_dims, latent_trajL.size(2)});

  #if DEBUG_PERFORM
      std::cout << "after width : " << latent_noiseL.sizes() << ";"
                << latent_trajL.sizes() << std::endl;
  #endif

      latent_trajL = torch::cat({latent_trajL, latent_noiseL}, 1);
      latent_trajR = torch::cat({latent_trajR, latent_noiseR}, 1);

  #if DEBUG_PERFORM
      std::cout << "latent processed" << std::endl;
  #endif

      latent_traj = torch::cat({latent_trajL, latent_trajR}, 0);
    }

    // Decode
    at::Tensor out = rave->decode(latent_traj);
    // On windows, I don't get why, but the two first dims are swapped (compared
    // to macOS / UNIX) with the same torch version
    if (out.sizes()[0] == 2) {
      out = out.transpose(0, 1);
    }

    const int outIndexR = (out.sizes()[1] > 1 ? 1 : 0);
    at::Tensor outL = out.index({0, 0, at::indexing::Slice()});
    at::Tensor outR = out.index({0, outIndexR, at::indexing::Slice()});

    if (outIndexR != 0) {
      const float width = juce::jlimit(0.0f, 2.0f, _widthValue->load() / 100.f);
      const at::Tensor mid = 0.5f * (outL + outR);
      const at::Tensor side = 0.5f * (outL - outR) * width;
      outL = mid + side;
      outR = mid - side;
    }

#if DEBUG_PERFORM
    std::cout << "latent decoded" << std::endl;
#endif

    float *outputDataPtrL, *outputDataPtrR;
    outputDataPtrL = outL.data_ptr<float>();
    outputDataPtrR = outR.data_ptr<float>();

    // Write in buffers
    assert(input_size >= 0);
    for (size_t i = 0; i < (size_t)input_size; i++) {
      _outModel[0][i] = outputDataPtrL[i];
      _outModel[1][i] = outputDataPtrR[i];
    }
    if (_smoothedFadeInOut.getCurrentValue() < EPSILON) {
      _isMuted.store(true);
    }
    } catch (const c10::Error &e) {
      juce::Logger::writeToLog("[latent_synth_v3] modelPerform c10::Error: " +
                               String(e.what_without_backtrace()));
      _isMuted.store(true);
    } catch (const std::exception &e) {
      juce::Logger::writeToLog("[latent_synth_v3] modelPerform std::exception: " +
                               String(e.what()));
      _isMuted.store(true);
    }
  }
}

void modelPerform_callback(RaveAP *ap) { ap->modelPerform(); }

void RaveAP::processBlock(juce::AudioBuffer<float> &buffer,
                          juce::MidiBuffer & /*midiMessages*/) {
  if (_isShuttingDown.load()) {
    buffer.clear();
    return;
  }

  juce::ScopedNoDenormals noDenormals;
  const int nSamples = buffer.getNumSamples();
  const int nChannels = buffer.getNumChannels();

  // mute if pause
  AudioPlayHead *playHead = this->getPlayHead();
  if (playHead != nullptr) {
    AudioPlayHead::CurrentPositionInfo info;
    bool hasDawInformation = playHead->getCurrentPosition(info);
    if (hasDawInformation) {
      bool isPlaying = info.isPlaying;
      _plays = isPlaying;
      if (isPlaying && _isMuted.load()) {
        unmute();
      } else if (!isPlaying && !_isMuted.load()) {
        mute();
        resetStreamingBuffers();
      }
    }
  }

  // fade parameters
  const muting muteConfig = _fadeScheduler.load();
  if (muteConfig == muting::mute) {
    _smoothedFadeInOut.setTargetValue(0.f);
  } else if (muteConfig == muting::unmute) {
    _smoothedFadeInOut.setTargetValue(1.f);
    _isMuted.store(false);
  }

  // Advance fade state every block so mute/unmute can complete.
  const float fadeValue = _smoothedFadeInOut.skip(nSamples);
  if (fadeValue < EPSILON) {
    _isMuted.store(true);
  }

  juce::dsp::AudioBlock<float> ab(buffer);
  juce::dsp::ProcessContextReplacing<float> context(ab);
  _compressorEffect.process(context);
  _inputGainEffect.process(context);
  _dryWetMixerEffect.pushDrySamples(ab);

  // Compute input RMS for the GUI
  _inputAmplitudeL = buffer.getRMSLevel(0, 0, nSamples);
  _inputAmplitudeR = buffer.getRMSLevel(1, 0, nSamples);

  // process input effects
  float *channelL = nullptr, *channelR = nullptr;
  if (nChannels == 1) {
    channelL = buffer.getWritePointer(0);
    channelR = channelL;
  } else {
    channelL = buffer.getWritePointer(0);
    channelR = buffer.getWritePointer(1);
  }

  juce::String channelMode =
      channel_modes[static_cast<int>(_channelMode->load()) - 1];
  if (channelMode == "L") {
    _inBuffer[0].put(channelL, nSamples);
  } else if (channelMode == "R") {
    _inBuffer[0].put(channelR, nSamples);
  } else if (channelMode == "L + R") {
    FloatVectorOperations::add(channelL, channelR, nSamples);
    FloatVectorOperations::multiply(channelL, 0.5f, nSamples);
    _inBuffer[0].put(channelL, nSamples);
  }

  // create processing thread
  int currentRefreshRate = pow(2, *_latencyMode);
  if (_computeThread && !_computeInFlight.load() && _computeThread->joinable()) {
    _computeThread->join();
    _computeThread.reset();
  }

  if (_inBuffer[0].len() >= currentRefreshRate) {
    if (!_computeInFlight.load() && _rave != nullptr) {
      _inBuffer[0].get(_inModel[0].get(), currentRefreshRate);
      _outBuffer[0].put(_outModel[0].get(), currentRefreshRate);
      _outBuffer[1].put(_outModel[1].get(), currentRefreshRate);
      _computeInFlight.store(true);
      _computeThread = std::make_unique<std::thread>([this]() {
        modelPerform();
        _computeInFlight.store(false);
      });
    }
  }

  AudioBuffer<float> out_buffer(2, nSamples);
  juce::dsp::AudioBlock<float> out_ab(out_buffer);
  juce::dsp::ProcessContextReplacing<float> out_context(out_ab);
  if (_outBuffer[0].len() >= nSamples) {
    _outBuffer[0].get(out_buffer.getWritePointer(0), nSamples);
    _outBuffer[1].get(out_buffer.getWritePointer(1), nSamples);
  } else {
    out_buffer.clear();
  }

#if DEBUG
  std::cout << "buffer out : " << out_buffer.getMagnitude(0, nSamples)
            << std::endl;
#endif

  _outputGainEffect.process(out_context);
  bool is_limiting = static_cast<bool>((*_limitValue).load());
  if (is_limiting)
    _limiterEffect.process(out_context);
  _dryWetMixerEffect.mixWetSamples(out_buffer);
  out_buffer.applyGain(fadeValue);
  buffer.copyFrom(0, 0, out_buffer, 0, 0, nSamples);
  if (nChannels == 2)
    buffer.copyFrom(1, 0, out_buffer, 1, 0, nSamples);
#if DEBUG_PERFORM
  std::cout << "sortie : " << buffer.getMagnitude(0, nSamples) << std::endl;
#endif
}

void RaveAP::parameterChanged(const String &parameterID, float newValue) {
  if (parameterID == rave_parameters::input_gain) {
    _inputGainEffect.setGainDecibels(newValue);
  } else if (parameterID == rave_parameters::input_ratio) {
    _compressorEffect.setRatio(newValue);
  } else if (parameterID == rave_parameters::input_thresh) {
    _compressorEffect.setThreshold(newValue);
  } else if (parameterID == rave_parameters::output_gain) {
    _outputGainEffect.setGainDecibels(newValue);
  } else if (parameterID == rave_parameters::output_drywet) {
    _dryWetMixerEffect.setWetMixProportion(newValue / 100.f);
  } else if (parameterID == rave_parameters::latency_mode) {
    auto latency_samples = std::pow(2.0f, _latencyMode->load());
    std::cout << "[ ] - latency has changed to " << latency_samples
              << std::endl;
    syncLatencyModeToSamples();
  }
}

void RaveAP::updateBufferSizes() {
  if (_rave == nullptr)
    return;
  const auto validBufferSizes = _rave->getValidBufferSizes();
  const auto minSamples = std::max(1, static_cast<int>(validBufferSizes.getStart()));
  const auto maxSamples = std::max(minSamples, static_cast<int>(validBufferSizes.getEnd()));
  const auto currentExponent = static_cast<int>(_latencyMode->load());
  const auto currentSamples = static_cast<int>(std::pow(2.0f, _latencyMode->load()));

  if (currentSamples < minSamples) {
    const auto targetExponent = static_cast<int>(std::log2((float)minSamples));
    std::cout << "too low; setting rate to : " << targetExponent << std::endl;
    setParameterValueNotifyingHost(rave_parameters::latency_mode,
                                   static_cast<float>(targetExponent));
  } else if (currentSamples > maxSamples) {
    const auto targetExponent = static_cast<int>(std::log2((float)maxSamples));
    std::cout << "too high; setting rate to : " << targetExponent << std::endl;
    setParameterValueNotifyingHost(rave_parameters::latency_mode,
                                   static_cast<float>(targetExponent));
  } else if (currentExponent >= 0) {
    syncLatencyModeToSamples();
  }
}

void RaveAP::updateEngine(const std::string modelFile) {
  if (_isShuttingDown.load())
    return;

  if (modelFile == _loadedModelName)
    return;
  _loadedModelName = modelFile;
  juce::ScopedLock irCalculationlock(_engineUpdateMutex);
  if (_engineThreadPool) {
    _engineThreadPool->removeAllJobs(true, 100);
  }

  _engineThreadPool->addJob(new UpdateEngineJob(*this, modelFile), true);
}

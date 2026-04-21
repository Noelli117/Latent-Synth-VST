#pragma once

#include "CircularBuffer.h"
#include "EngineUpdater.h"
#include "LatentFlowController.h"
#include "Rave.h"
#include <JuceHeader.h>
#include <algorithm>
#include <torch/script.h>
#include <torch/torch.h>

#define EPSILON 0.0000001
#define DEBUG 0

const size_t AVAILABLE_DIMS = 8;
const juce::StringArray channel_modes = {"L", "R", "L + R"};

namespace rave_parameters {
const String model_selection{"model_selection"};
const String external_latent_mode{"external_latent_mode"};
const String external_latent_value{"external_latent_value"};
const String input_gain{"input_gain"};
const String channel_mode{"channel_mode"};
const String input_thresh{"input_threshold"};
const String input_ratio{"input_ratio"};
const String latent_jitter{"latent_jitter"};
const String output_width{"output_width"};
const String output_gain{"output_gain"};
const String output_limit{"output_limit"};
const String output_drywet{"ouptut_drywet"};
const String latent_scale{"latent_scale"};
const String latent_bias{"latent_bias"};
const String latency_mode{"latency_mode"};
const String use_prior{"use_prior"};
const String prior_temperature{"prior_temperature"};
const String flow_speed{"flow_speed"};
const String flow_noise_scale{"flow_noise_scale"};
const String flow_curve{"flow_curve"};
const String flow_gain{"flow_gain"};
const String flow_contrast{"flow_contrast"};
const String flow_intensity{"flow_intensity"};
} // namespace rave_parameters

namespace rave_ranges {
const NormalisableRange<float> gainRange(-70.f, 12.f);
const NormalisableRange<float> latentScaleRange(0.0f, 5.0f);
const NormalisableRange<float> latentBiasRange(-3.0f, 3.0f);
} // namespace rave_ranges

class NAAudioParameterInt: public juce::AudioParameterInt {
  public: 
    NAAudioParameterInt(const String &parameterID,
                        const String &parameterName,
                        int minValue, int maxValue, 
                        int defaultValue, const String &parameterLabel=String(), 
                        std::function< String(int value, int maximumStringLength)> stringFromInt=nullptr,
                        std::function< int(const String &text)> intFromString=nullptr): 
                        juce::AudioParameterInt(parameterID, parameterName, minValue, maxValue, defaultValue, parameterLabel, stringFromInt, intFromString)  
    {}

    bool isAutomatable() const override { return false; }
};


class RaveAP : public juce::AudioProcessor,
               public juce::AudioProcessorValueTreeState::Listener {
  // WARNING: As we do not implement processBlock() without parameters like in:
  // https://docs.juce.com/master/classAudioProcessor.html#abbac77f68ba047cf60c4bc97326dcb58
  // we explicitely authorize the use of AudioProcessor's processBlock
  // implementation through RaveAP. See:
  // https://stackoverflow.com/questions/9995421/gcc-woverloaded-virtual-warnings
  using juce::AudioProcessor::processBlock;

public:
  RaveAP();
  ~RaveAP() override;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;
#ifndef JucePlugin_PreferredChannelConfigurations
  bool isBusesLayoutSupported(const BusesLayout &layouts) const override;
#endif
  void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
  void modelPerform();
  void detectAvailableModels();
  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override;
  const juce::String getName() const override;
  bool acceptsMidi() const override;
  bool producesMidi() const override;
  bool isMidiEffect() const override;
  double getTailLengthSeconds() const override;
  int getNumPrograms() override;
  int getCurrentProgram() override;
  void setCurrentProgram(int index) override;
  const juce::String getProgramName(int index) override;
  void changeProgramName(int index, const juce::String &newName) override;
  AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
  void getStateInformation(juce::MemoryBlock &destData) override;
  void setStateInformation(const void *data, int sizeInBytes) override;
  void parameterChanged(const String &parameterID, float newValue) override;

  auto mute() -> void;
  auto unmute() -> void;
  auto getIsMuted() -> const bool;
  bool isShuttingDown() const;
  bool isComputeInFlight() const;
  void updateBufferSizes();

  void updateEngine(const std::string modelFile);
  std::string capitalizeFirstLetter(std::string text);
  float getAmplitude(float *buffer, size_t len);

  std::unique_ptr<RAVE> _rave;
  float _inputAmplitudeL;
  float _inputAmplitudeR;
  float _outputAmplitudeL;
  float _outputAmplitudeR;
  bool _plays = false; 

  double getSampleRate() { return _sampleRate; }
  void setExternalLatentMode(bool enabled);
  bool getExternalLatentMode() const;
  void setExternalLatentValue(size_t index, float value);
  float getExternalLatentValue(size_t index) const;
  void setLatentScaleValue(size_t index, float value);
  void setLatentBiasValue(size_t index, float value);
  float getLatentScaleValue(size_t index) const;
  float getLatentBiasValue(size_t index) const;
  void setWebUiFlowSpeed(float value);
  void setWebUiFlowNoiseScale(float value);
  void setWebUiFlowCurve(float value);
  void setWebUiFlowGain(float value);
  void setWebUiFlowContrast(float value);
  void setWebUiFlowIntensity(float value);
  float getWebUiFlowSpeed() const;
  float getWebUiFlowNoiseScale() const;
  float getWebUiFlowCurve() const;
  float getWebUiFlowGain() const;
  float getWebUiFlowContrast() const;
  float getWebUiFlowIntensity() const;
  float getWebUiFlowRadius() const;

private:
  void persistExternalLatentMode();
  void persistExternalLatentValue(size_t index);
  void persistWebUiFlowState(const juce::String &propertyName, float value);
  void restorePersistentLatentState();
  void syncLatencyModeToSamples();
  void resetStreamingBuffers();
  void updateRuntimeExternalLatentValues(
      const std::array<float, AVAILABLE_DIMS> &values);
  void setParameterValueNotifyingHost(const juce::String &parameterID,
                                      float value);
  mutable CriticalSection _engineUpdateMutex;
  juce::AudioProcessorValueTreeState _avts;
  std::unique_ptr<juce::ThreadPool> _engineThreadPool;
  std::string _loadedModelName;

  /*
   *Allocate some memory to use as the circular_buffer storage
   *for each of the circular_buffer types to be created
   */
  double _sampleRate = 0;
  std::unique_ptr<circular_buffer<float, float>[]> _inBuffer;
  std::unique_ptr<circular_buffer<float, float>[]> _outBuffer;
  std::vector<std::unique_ptr<float[]>> _inModel, _outModel;
  std::unique_ptr<std::thread> _computeThread;
  std::atomic<bool> _computeInFlight{false};

  bool _editorReady;

  float *_inFifoBuffer{nullptr};
  float *_outFifoBuffer{nullptr};

  std::atomic<float> *_inputGainValue;
  std::atomic<float> *_thresholdValue;
  std::atomic<float> *_ratioValue;
  std::atomic<float> *_latentJitterValue;
  std::atomic<float> *_widthValue;
  std::atomic<float> *_outputGainValue;
  std::atomic<float> *_dryWetValue;
  std::atomic<float> *_limitValue;
  std::atomic<float> *_channelMode;
  // latency mode contains the power of 2 of the current refresh rate.
  std::atomic<float> *_latencyMode;
  std::atomic<float> *_usePrior;
  std::atomic<float> *_priorTemperature;

  std::array<std::atomic<float> *, AVAILABLE_DIMS> *_latentScale;
  std::array<std::atomic<float> *, AVAILABLE_DIMS> *_latentBias;
  std::atomic<bool> _externalLatentMode{false};
  std::array<std::atomic<float>, AVAILABLE_DIMS> _externalLatentValues;
  LatentFlowController _latentFlowController;
  std::atomic<float> _webUiFlowSpeed{5.0f};
  std::atomic<float> _webUiFlowNoiseScale{0.0255f};
  std::atomic<float> _webUiFlowCurve{2.25f};
  std::atomic<float> _webUiFlowGain{0.155f};
  std::atomic<float> _webUiFlowContrast{0.475f};
  std::atomic<float> _webUiFlowIntensity{0.65f};
  std::atomic<float> _webUiFlowRadius{0.0f};
  std::atomic<bool> _isMuted{true};
  std::atomic<bool> _isShuttingDown{false};

  enum class muting : int { ignore = 0, mute, unmute };

  std::atomic<muting> _fadeScheduler{muting::mute};
  LinearSmoothedValue<float> _smoothedFadeInOut;
  LinearSmoothedValue<float> _smoothedWetGain;
  LinearSmoothedValue<float> _smoothedDryGain;

  // DSP effect
  juce::dsp::Compressor<float> _compressorEffect;
  juce::dsp::Gain<float> _inputGainEffect;
  juce::dsp::Gain<float> _outputGainEffect;
  juce::dsp::Limiter<float> _limiterEffect;
  juce::dsp::DryWetMixer<float> _dryWetMixerEffect;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RaveAP)
};

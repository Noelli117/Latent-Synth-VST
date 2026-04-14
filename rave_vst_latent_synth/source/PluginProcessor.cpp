#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <math.h>

RaveAP::RaveAP()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      _avts(*this, nullptr, Identifier("RAVEValueTree"),
            createParameterLayout()),
      _loadedModelName(""), _computeThread(nullptr), _dryWetMixerEffect(BUFFER_LENGTH)
#endif
{
  _inBuffer = std::make_unique<circular_buffer<float, float>[]>(1);
  _outBuffer = std::make_unique<circular_buffer<float, float>[]>(2);
  _inModel.push_back(std::make_unique<float[]>(BUFFER_LENGTH));
  _outModel.push_back(std::make_unique<float[]>(BUFFER_LENGTH));
  _outModel.push_back(std::make_unique<float[]>(BUFFER_LENGTH));

  _inputGainValue = _avts.getRawParameterValue(rave_parameters::input_gain);
  _thresholdValue = _avts.getRawParameterValue(rave_parameters::input_thresh);
  _ratioValue = _avts.getRawParameterValue(rave_parameters::input_ratio);
  _channelMode = _avts.getRawParameterValue(rave_parameters::channel_mode);
  _latentJitterValue =
      _avts.getRawParameterValue(rave_parameters::latent_jitter);
  _widthValue = _avts.getRawParameterValue(rave_parameters::output_width);
  _outputGainValue = _avts.getRawParameterValue(rave_parameters::output_gain);
  _dryWetValue = _avts.getRawParameterValue(rave_parameters::output_drywet);
  _limitValue = _avts.getRawParameterValue(rave_parameters::output_limit);
  _usePrior = _avts.getRawParameterValue(rave_parameters::use_prior);
  _latentScale = new std::array<std::atomic<float> *, AVAILABLE_DIMS>;
  _latentBias = new std::array<std::atomic<float> *, AVAILABLE_DIMS>;
  for (unsigned long i = 0; i < AVAILABLE_DIMS; i++) {
    (*_latentScale)[i] = _avts.getRawParameterValue(
        rave_parameters::latent_scale + String("_") + std::to_string(i));
    (*_latentBias)[i] = _avts.getRawParameterValue(
        rave_parameters::latent_bias + String("_") + std::to_string(i));
  }
  _latencyMode = _avts.getRawParameterValue(rave_parameters::latency_mode);
  _priorTemperature = _avts.getRawParameterValue(rave_parameters::prior_temperature);
  _engineThreadPool = std::make_unique<ThreadPool>(1);
  _rave.reset(new RAVE());
  for (size_t i = 0; i < AVAILABLE_DIMS; ++i) {
    _externalLatentValues[i].store(0.0f);
  }
  restorePersistentLatentState();

  _avts.addParameterListener(rave_parameters::input_gain, this);
  _avts.addParameterListener(rave_parameters::input_thresh, this);
  _avts.addParameterListener(rave_parameters::input_ratio, this);
  _avts.addParameterListener(rave_parameters::output_gain, this);
  _avts.addParameterListener(rave_parameters::output_limit, this);
  _avts.addParameterListener(rave_parameters::output_drywet, this);
  _avts.addParameterListener(rave_parameters::latency_mode, this);
  _dryWetMixerEffect.setMixingRule(juce::dsp::DryWetMixingRule::balanced);
  _editorReady = false;
}

RaveAP::~RaveAP() {
  _isShuttingDown.store(true);

  if (_engineThreadPool) {
    _engineThreadPool->removeAllJobs(true, -1);
    _engineThreadPool.reset();
  }

  if (_computeThread && _computeThread->joinable()) {
    _computeThread->join();
    _computeThread.reset();
  }

  if (_rave != nullptr) {
    _rave->dispatchPendingMessages();
    _rave->removeAllChangeListeners();
  }
}

void RaveAP::prepareToPlay(double sampleRate, int samplesPerBlock) {
  _sampleRate = sampleRate;
  _inBuffer[0].initialize(BUFFER_LENGTH);
  _outBuffer[0].initialize(BUFFER_LENGTH);
  _outBuffer[1].initialize(BUFFER_LENGTH);
  _latentFlowController.reset();
  _webUiFlowRadius.store(0.0f);
  _smoothedFadeInOut.reset(sampleRate, 0.2);
  juce::dsp::ProcessSpec specs = {
      sampleRate, static_cast<juce::uint32>(samplesPerBlock), 2};
  _inputGainEffect.prepare(specs);
  _compressorEffect.prepare(specs);
  _outputGainEffect.prepare(specs);
  _limiterEffect.prepare(specs);
  _limiterEffect.setThreshold(-1.f);
  _dryWetMixerEffect.prepare(specs);
  _inputGainEffect.setGainDecibels(_inputGainValue->load());
  _compressorEffect.setRatio(_ratioValue->load());
  _compressorEffect.setThreshold(_thresholdValue->load());
  _outputGainEffect.setGainDecibels(_outputGainValue->load());
  _dryWetMixerEffect.setWetMixProportion(_dryWetValue->load() / 100.f);
  const auto initialLatencySamples =
      static_cast<int>(pow(2, *_latencyMode));
  _dryWetMixerEffect.setWetLatency(initialLatencySamples);
  setLatencySamples(initialLatencySamples);
}

void RaveAP::releaseResources() {
  // When playback stops, you can use this as an opportunity to free up any
  // spare memory, etc.
  _limiterEffect.reset();
  _inputGainEffect.reset();
  _outputGainEffect.reset();
  _compressorEffect.reset();
  _dryWetMixerEffect.reset();
}

juce::String valueToTextFunction(float value) {
    return juce::String(value);
}

float textToValueFunction (const String &value) {
    return value.getFloatValue();
}



AudioProcessorValueTreeState::ParameterLayout RaveAP::createParameterLayout() {
  std::vector<std::unique_ptr<RangedAudioParameter>> params;
  params.push_back(std::make_unique<AudioParameterFloat>(
      rave_parameters::input_gain, rave_parameters::input_gain,
      rave_ranges::gainRange, 0.f));
  params.push_back(std::make_unique<AudioParameterFloat>(
      rave_parameters::input_thresh, rave_parameters::input_thresh, -60.f, 0.f,
      0.f));
  params.push_back(std::make_unique<AudioParameterFloat>(
      rave_parameters::input_ratio, rave_parameters::input_ratio, 1.f, 10.f,
      1.f));
  params.push_back(std::make_unique<AudioParameterInt>(
      rave_parameters::channel_mode, rave_parameters::channel_mode, 1, 3, 1));
  params.push_back(std::make_unique<AudioParameterFloat>(
      rave_parameters::latent_jitter, rave_parameters::latent_jitter, 0.f, 3.f,
      0.f));
  params.push_back(std::make_unique<AudioParameterFloat>(
      rave_parameters::output_width, rave_parameters::output_width, 0.f, 200.f,
      100.f));
  params.push_back(std::make_unique<AudioParameterFloat>(
      rave_parameters::output_gain, rave_parameters::output_gain,
      rave_ranges::gainRange, 0.f));
  params.push_back(std::make_unique<AudioParameterFloat>(
      rave_parameters::output_drywet, rave_parameters::output_drywet, 0.f,
      100.f, 100.f));
  params.push_back(std::make_unique<AudioParameterBool>(
      rave_parameters::output_limit, rave_parameters::output_limit, true));
  params.push_back(std::make_unique<NAAudioParameterInt>(
      rave_parameters::latency_mode, rave_parameters::latency_mode, 9, 15, 13));
  params.push_back(std::make_unique<AudioParameterBool>(
      rave_parameters::use_prior, rave_parameters::use_prior, true));
  params.push_back(std::make_unique<AudioParameterFloat>(
      rave_parameters::prior_temperature, rave_parameters::prior_temperature,
      0.f, 5.f, 1.f));

  String current_name;
  for (size_t i = 0; i < AVAILABLE_DIMS; i++) {
    current_name =
        rave_parameters::latent_scale + String("_") + std::to_string(i);
    params.push_back(std::make_unique<AudioParameterFloat>(
        current_name, current_name, rave_ranges::latentScaleRange, 1.0));
    current_name =
        rave_parameters::latent_bias + String("_") + std::to_string(i);
    params.push_back(std::make_unique<AudioParameterFloat>(
        current_name, current_name, rave_ranges::latentBiasRange, 0.0));
  }
  return {params.begin(), params.end()};
}

void RaveAP::setExternalLatentMode(bool enabled) {
  _externalLatentMode.store(enabled);
  persistExternalLatentMode();
}

bool RaveAP::getExternalLatentMode() const {
  return _externalLatentMode.load();
}

bool RaveAP::isShuttingDown() const { return _isShuttingDown.load(); }

bool RaveAP::isComputeInFlight() const { return _computeInFlight.load(); }

void RaveAP::setExternalLatentValue(size_t index, float value) {
  if (index >= AVAILABLE_DIMS) {
    return;
  }
  _externalLatentValues[index].store(value);
  persistExternalLatentValue(index);
}

float RaveAP::getExternalLatentValue(size_t index) const {
  if (index >= AVAILABLE_DIMS) {
    return 0.0f;
  }
  return _externalLatentValues[index].load();
}

void RaveAP::setLatentScaleValue(size_t index, float value) {
  if (index >= AVAILABLE_DIMS || _latentScale == nullptr) {
    return;
  }
  const float clamped =
      juce::jlimit(rave_ranges::latentScaleRange.start,
                   rave_ranges::latentScaleRange.end, value);
  setParameterValueNotifyingHost(
      rave_parameters::latent_scale + String("_") + std::to_string(index),
      clamped);
}

void RaveAP::setLatentBiasValue(size_t index, float value) {
  if (index >= AVAILABLE_DIMS || _latentBias == nullptr) {
    return;
  }
  const float clamped =
      juce::jlimit(rave_ranges::latentBiasRange.start,
                   rave_ranges::latentBiasRange.end, value);
  setParameterValueNotifyingHost(
      rave_parameters::latent_bias + String("_") + std::to_string(index),
      clamped);
}

float RaveAP::getLatentScaleValue(size_t index) const {
  if (index >= AVAILABLE_DIMS || _latentScale == nullptr) {
    return 1.0f;
  }
  return (*_latentScale)[index]->load();
}

float RaveAP::getLatentBiasValue(size_t index) const {
  if (index >= AVAILABLE_DIMS || _latentBias == nullptr) {
    return 0.0f;
  }
  return (*_latentBias)[index]->load();
}

void RaveAP::setWebUiFlowSpeed(float value) {
  const float clamped = juce::jlimit(0.0f, 10.0f, value);
  _webUiFlowSpeed.store(clamped);
  persistWebUiFlowState(rave_parameters::flow_speed, clamped);
}

void RaveAP::setWebUiFlowNoiseScale(float value) {
  const float clamped = juce::jlimit(0.001f, 0.05f, value);
  _webUiFlowNoiseScale.store(clamped);
  persistWebUiFlowState(rave_parameters::flow_noise_scale, clamped);
}

void RaveAP::setWebUiFlowCurve(float value) {
  const float clamped = juce::jlimit(0.5f, 4.0f, value);
  _webUiFlowCurve.store(clamped);
  persistWebUiFlowState(rave_parameters::flow_curve, clamped);
}

void RaveAP::setWebUiFlowGain(float value) {
  const float clamped = juce::jlimit(0.01f, 0.3f, value);
  _webUiFlowGain.store(clamped);
  persistWebUiFlowState(rave_parameters::flow_gain, clamped);
}

void RaveAP::setWebUiFlowContrast(float value) {
  const float clamped = juce::jlimit(0.0f, 0.95f, value);
  _webUiFlowContrast.store(clamped);
  persistWebUiFlowState(rave_parameters::flow_contrast, clamped);
}

void RaveAP::setWebUiFlowIntensity(float value) {
  const float clamped = juce::jlimit(0.3f, 1.0f, value);
  _webUiFlowIntensity.store(clamped);
  persistWebUiFlowState(rave_parameters::flow_intensity, clamped);
}

float RaveAP::getWebUiFlowSpeed() const { return _webUiFlowSpeed.load(); }

float RaveAP::getWebUiFlowNoiseScale() const {
  return _webUiFlowNoiseScale.load();
}

float RaveAP::getWebUiFlowCurve() const { return _webUiFlowCurve.load(); }

float RaveAP::getWebUiFlowGain() const { return _webUiFlowGain.load(); }

float RaveAP::getWebUiFlowContrast() const {
  return _webUiFlowContrast.load();
}

float RaveAP::getWebUiFlowIntensity() const {
  return _webUiFlowIntensity.load();
}

float RaveAP::getWebUiFlowRadius() const { return _webUiFlowRadius.load(); }

void RaveAP::persistExternalLatentMode() {
  _avts.state.setProperty(rave_parameters::external_latent_mode,
                          _externalLatentMode.load(), nullptr);
}

void RaveAP::persistExternalLatentValue(size_t index) {
  if (index >= AVAILABLE_DIMS) {
    return;
  }
  _avts.state.setProperty(rave_parameters::external_latent_value +
                              String("_") + std::to_string(index),
                          _externalLatentValues[index].load(), nullptr);
}

void RaveAP::persistWebUiFlowState(const juce::String &propertyName,
                                   float value) {
  _avts.state.setProperty(propertyName, value, nullptr);
}

void RaveAP::restorePersistentLatentState() {
  if (_avts.state.hasProperty(rave_parameters::external_latent_mode)) {
    _externalLatentMode.store(static_cast<bool>(
        _avts.state.getProperty(rave_parameters::external_latent_mode)));
  }

  for (size_t i = 0; i < AVAILABLE_DIMS; ++i) {
    const auto propertyName =
        rave_parameters::external_latent_value + String("_") + std::to_string(i);
    if (_avts.state.hasProperty(propertyName)) {
      _externalLatentValues[i].store(
          static_cast<float>(_avts.state.getProperty(propertyName)));
    }
  }

  if (_avts.state.hasProperty(rave_parameters::flow_speed)) {
    _webUiFlowSpeed.store(
        (float)_avts.state.getProperty(rave_parameters::flow_speed));
  }
  if (_avts.state.hasProperty(rave_parameters::flow_noise_scale)) {
    _webUiFlowNoiseScale.store(
        (float)_avts.state.getProperty(rave_parameters::flow_noise_scale));
  }
  if (_avts.state.hasProperty(rave_parameters::flow_curve)) {
    _webUiFlowCurve.store(
        (float)_avts.state.getProperty(rave_parameters::flow_curve));
  }
  if (_avts.state.hasProperty(rave_parameters::flow_gain)) {
    _webUiFlowGain.store(
        (float)_avts.state.getProperty(rave_parameters::flow_gain));
  }
  if (_avts.state.hasProperty(rave_parameters::flow_contrast)) {
    _webUiFlowContrast.store(
        (float)_avts.state.getProperty(rave_parameters::flow_contrast));
  }
  if (_avts.state.hasProperty(rave_parameters::flow_intensity)) {
    _webUiFlowIntensity.store(
        (float)_avts.state.getProperty(rave_parameters::flow_intensity));
  }
}

void RaveAP::updateRuntimeExternalLatentValues(
    const std::array<float, AVAILABLE_DIMS> &values) {
  for (size_t i = 0; i < AVAILABLE_DIMS; ++i) {
    _externalLatentValues[i].store(values[i]);
  }
}

void RaveAP::setParameterValueNotifyingHost(const juce::String &parameterID,
                                            float value) {
  if (auto *param = _avts.getParameter(parameterID)) {
    const auto range = param->getNormalisableRange();
    const float clamped = juce::jlimit(range.start, range.end, value);
    param->beginChangeGesture();
    param->setValueNotifyingHost(range.convertTo0to1(clamped));
    param->endChangeGesture();
  }
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  // This creates new instances of the plugin..
  return new RaveAP();
}

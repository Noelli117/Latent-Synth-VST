#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

class LatentFlowController {
public:
  static constexpr size_t kLatentDim = 8;

  struct Params {
    float speed = 5.0f;
    float noiseScale = 0.0255f;
    float curve = 2.25f;
    float gain = 0.155f;
    float contrast = 0.475f;
    float intensity = 0.65f;
  };

  LatentFlowController();

  void reset(uint32 seed = 0);
  void setParams(const Params &params);
  void step(float dtSeconds);

  const std::array<float, kLatentDim> &getLatent() const;
  float getRadius() const;

private:
  struct Particle {
    float x = 0.0f;
    float y = 0.0f;
  };

  void simulateFrame();
  float sampleNoise2D(float x, float y) const;
  float hashNoise(int x, int y) const;
  float gaussianNoise();

  static float lerp(float a, float b, float t);
  static float map(float value, float inMin, float inMax, float outMin,
                   float outMax);

  static constexpr float kEpsilon = 1.0e-6f;
  static constexpr float kPi = 3.14159265358979323846f;
  static constexpr int kBaseParticles = 128;
  static constexpr int kMaxParticles = 320;
  static constexpr float kLatentRadiusMin = 0.05f;
  static constexpr float kLatentRadiusMax = 8.0f;
  static constexpr float kLatentNoiseMax = 4.0f;
  static constexpr float kTemporalJitterOnset = 0.6f;
  static constexpr float kCanvasWidth = 1000.0f;
  static constexpr float kCanvasHeight = 700.0f;
  static constexpr float kReferenceFrameRate = 60.0f;

  juce::Random _random;
  Params _params;
  std::vector<Particle> _particles;
  std::array<float, kLatentDim> _latent{};
  std::array<float, kLatentDim> _targetLatent{};
  std::array<float, kLatentDim> _latentNoiseState{};
  std::array<float, kLatentDim> _directionUnit{};
  float _timeSeconds = 0.0f;
  float _radius = 0.0f;
};

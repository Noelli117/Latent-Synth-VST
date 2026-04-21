#include "LatentFlowController.h"

#include <algorithm>
#include <cmath>

LatentFlowController::LatentFlowController() { reset(); }

void LatentFlowController::reset(uint32 seed) {
  _random.setSeedRandomly();
  if (seed != 0) {
    _random.setSeed(seed);
  }

  _particles.resize((size_t)kMaxParticles);
  for (auto &particle : _particles) {
    particle.x = _random.nextFloat() * kCanvasWidth;
    particle.y = _random.nextFloat() * kCanvasHeight;
  }

  for (size_t i = 0; i < kLatentDim; ++i) {
    _latent[i] = _random.nextFloat() * 2.0f - 1.0f;
    _targetLatent[i] = _latent[i];
    _latentNoiseState[i] = 0.0f;
    _directionUnit[i] = 0.0f;
  }

  _timeSeconds = 0.0f;
  _radius = 0.0f;
}

void LatentFlowController::setParams(const Params &params) { _params = params; }

void LatentFlowController::step(float dtSeconds) {
  if (_particles.empty()) {
    reset();
  }

  const float safeDt = std::max(dtSeconds, 1.0f / kReferenceFrameRate);
  const int substeps =
      juce::jlimit(1, 32, (int)std::round(safeDt * kReferenceFrameRate));
  for (int i = 0; i < substeps; ++i) {
    simulateFrame();
    _timeSeconds += 1.0f / kReferenceFrameRate;
  }
}

const std::array<float, LatentFlowController::kLatentDim> &
LatentFlowController::getLatent() const {
  return _latent;
}

float LatentFlowController::getRadius() const { return _radius; }

void LatentFlowController::simulateFrame() {
  const int activeParticles = juce::jlimit(
      kBaseParticles, kMaxParticles,
      (int)std::floor(lerp((float)kBaseParticles, (float)kMaxParticles,
                           _params.intensity)));
  const float speed = _params.speed;
  const float noiseScale = _params.noiseScale;
  const float curviness = _params.curve;
  const float flowGain = _params.gain;
  const float contrast = _params.contrast;
  const float intensity = _params.intensity;

  const float S = speed * flowGain;
  const float safeS = std::max(S, kEpsilon);
  const float sx = 1.0f - contrast;
  const float sy = 1.0f + contrast;
  const float invNum = 1.0f / (float)activeParticles;

  float sumVX = 0.0f;
  float sumVY = 0.0f;
  float sumPX = 0.0f;
  float sumPY = 0.0f;
  float sumNoise = 0.0f;
  float sumCos = 0.0f;
  float sumSin = 0.0f;
  float sumDriveMag = 0.0f;

  for (int i = 0; i < activeParticles; ++i) {
    auto &particle = _particles[(size_t)i];
    const float n =
        sampleNoise2D(particle.x * noiseScale, particle.y * noiseScale);
    const float a = kPi * curviness * (n - 0.5f);

    const float tx = std::cos(a) * sx;
    const float ty = std::sin(a) * sy;
    const float driveMag = std::sqrt(tx * tx + ty * ty);
    sumDriveMag += driveMag;

    const float magnitude = driveMag + kEpsilon;
    const float invMagnitude = 1.0f / magnitude;

    const float vx = tx * invMagnitude * S;
    const float vy = ty * invMagnitude * S;

    particle.x += vx;
    particle.y += vy;

    sumVX += vx;
    sumVY += vy;
    sumPX += particle.x;
    sumPY += particle.y;
    sumNoise += n;
    sumCos += tx * invMagnitude;
    sumSin += ty * invMagnitude;

    if (particle.x < 0.0f || particle.x > kCanvasWidth || particle.y < 0.0f ||
        particle.y > kCanvasHeight) {
      particle.x = _random.nextFloat() * kCanvasWidth;
      particle.y = _random.nextFloat() * kCanvasHeight;
    }
  }

  const float meanVX = sumVX * invNum;
  const float meanVY = sumVY * invNum;
  const float meanDriveMag = sumDriveMag * invNum;
  const float maxDriveMag = std::max(std::abs(sx), std::abs(sy)) + kEpsilon;
  const float motionNorm = juce::jlimit(0.0f, 1.0f, meanDriveMag / maxDriveMag);
  const float motionEnergy = motionNorm * safeS;

  float rTarget =
      map(motionEnergy, 0.0f, safeS, kLatentRadiusMin, kLatentRadiusMax);
  rTarget = juce::jlimit(kLatentRadiusMin, kLatentRadiusMax, rTarget);

  const float avgX = sumPX * invNum;
  const float avgY = sumPY * invNum;
  const float avgNoise = sumNoise * invNum;
  const float avgAngle = std::atan2(sumSin * invNum, sumCos * invNum);

  std::array<float, kLatentDim> direction{};
  direction[0] = map(avgX, 0.0f, kCanvasWidth, -1.0f, 1.0f);
  direction[1] = map(avgY, 0.0f, kCanvasHeight, -1.0f, 1.0f);
  direction[2] = avgNoise * 2.0f - 1.0f;
  direction[3] = avgAngle / kPi;
  direction[4] = map(meanVX, -safeS, safeS, -1.0f, 1.0f);
  direction[5] = map(meanVY, -safeS, safeS, -1.0f, 1.0f);
  direction[6] = map(speed, 0.0f, 10.0f, -1.0f, 1.0f);
  direction[7] = map(curviness, 0.5f, 4.0f, -1.0f, 1.0f);

  float norm = 0.0f;
  for (float value : direction) {
    norm += value * value;
  }
  norm = std::sqrt(norm) + kEpsilon;
  for (size_t i = 0; i < kLatentDim; ++i) {
    _directionUnit[i] = direction[i] / norm;
  }

  const float temporalJitterMix =
      juce::jlimit(0.0f, 1.0f,
                   (intensity - kTemporalJitterOnset) /
                       (1.0f - kTemporalJitterOnset));
  const float motionJitterAmount =
      intensity * motionNorm * 0.6f * temporalJitterMix;
  const float latentNoiseAmount = intensity * kLatentNoiseMax;
  const float noiseSpeed = lerp(0.03f, 0.6f, intensity);

  for (size_t i = 0; i < kLatentDim; ++i) {
    const float global = rTarget * _directionUnit[i];
    _targetLatent[i] = global;

    const float temporalJitter =
        (sampleNoise2D(_timeSeconds * 1.2f, (float)i * 31.7f) - 0.5f) * 2.0f;
    _targetLatent[i] += motionJitterAmount * temporalJitter;

    _latentNoiseState[i] =
        lerp(_latentNoiseState[i], gaussianNoise(), noiseSpeed);
    _targetLatent[i] += latentNoiseAmount * _latentNoiseState[i];
  }

  const float alpha = lerp(0.01f, 0.25f, intensity);
  float radiusSq = 0.0f;
  for (size_t i = 0; i < kLatentDim; ++i) {
    _latent[i] += alpha * (_targetLatent[i] - _latent[i]);
    radiusSq += _latent[i] * _latent[i];
  }
  _radius = std::sqrt(radiusSq);
}

float LatentFlowController::sampleNoise2D(float x, float y) const {
  const int x0 = (int)std::floor(x);
  const int y0 = (int)std::floor(y);
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;

  const float tx = x - (float)x0;
  const float ty = y - (float)y0;
  const float fadeX = tx * tx * (3.0f - 2.0f * tx);
  const float fadeY = ty * ty * (3.0f - 2.0f * ty);

  const float n00 = hashNoise(x0, y0);
  const float n10 = hashNoise(x1, y0);
  const float n01 = hashNoise(x0, y1);
  const float n11 = hashNoise(x1, y1);

  const float nx0 = lerp(n00, n10, fadeX);
  const float nx1 = lerp(n01, n11, fadeX);
  return lerp(nx0, nx1, fadeY);
}

float LatentFlowController::hashNoise(int x, int y) const {
  uint32 h = (uint32)x * 374761393u + (uint32)y * 668265263u;
  h ^= h >> 13;
  h *= 1274126177u;
  h ^= h >> 16;
  return (float)(h & 0x00ffffffu) / (float)0x01000000u;
}

float LatentFlowController::gaussianNoise() {
  float u = 0.0f;
  float v = 0.0f;
  while (u <= 0.0f) {
    u = _random.nextFloat();
  }
  while (v <= 0.0f) {
    v = _random.nextFloat();
  }
  return std::sqrt(-2.0f * std::log(u)) * std::cos(2.0f * kPi * v);
}

float LatentFlowController::lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

float LatentFlowController::map(float value, float inMin, float inMax,
                                float outMin, float outMax) {
  const float inSpan = inMax - inMin;
  if (std::abs(inSpan) < kEpsilon) {
    return outMin;
  }
  const float normalized = (value - inMin) / inSpan;
  return outMin + normalized * (outMax - outMin);
}

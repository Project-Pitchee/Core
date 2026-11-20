#ifndef PITCHEE_V0_AUDIO_HPP
#define PITCHEE_V0_AUDIO_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pitchee::v0 {

constexpr int kSampleRate = 16000;
constexpr int kFrameSamples = 320;
constexpr int kHopSamples = 160;
constexpr int kMaximumSamples = 12 * kSampleRate;
constexpr int kMinimumSamples = 6400;

struct AudioMetadata {
    double source_seconds = 0.0;
    double analyzed_seconds = 0.0;
    double speech_seconds = 0.0;
    int active_frames = 0;
    int64_t crop_start_sample = 0;
};

struct ProcessedAudio {
    std::vector<float> samples;
    AudioMetadata metadata;
};

std::vector<float> resample_poly(
    const float* input,
    size_t input_samples,
    int input_rate,
    int output_rate
);

std::vector<float> average_channels(
    const float* interleaved,
    size_t sample_count,
    int channels
);

ProcessedAudio prepare_audio(
    const float* interleaved,
    size_t sample_count,
    int sample_rate,
    int channels
);

std::vector<float> normalize_waveform(const std::vector<float>& samples);

}  // namespace pitchee::v0

#endif

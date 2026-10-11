#ifndef PITCHEE_HNR_HPP
#define PITCHEE_HNR_HPP

#include "../include/pitchee/hnr.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace pitchee {

constexpr int kHNRSampleRate = 16000;
constexpr size_t kHNRWindowSamples = 640;
constexpr size_t kHNRHopSamples = 160;
constexpr double kHNRWindowSeconds = 0.04;
constexpr const char* kHNRAlgorithm = "autocorr-praat-v1";

struct HNRWindow {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    std::optional<double> hnr_db;
};

struct VoiceQualityResult {
    std::optional<double> hnr_db;
    int hnr_window_count = 0;
    std::optional<double> hnr_std_db;
    double window_seconds = kHNRWindowSeconds;
    std::vector<HNRWindow> windows;
};

// Pure PCM helpers: no model, F0, heap allocation, or retained input pointer.
std::optional<double> hnr_from_autocorrelation(double correlation) noexcept;
HNRWindow calculate_hnr_window(
    const float* samples,
    size_t sample_count,
    size_t start_sample = 0
) noexcept;
void summarize_hnr(VoiceQualityResult& result) noexcept;

// Offline convenience wrapper; only storing the output timeline allocates.
VoiceQualityResult analyze_hnr(
    const float* samples,
    size_t sample_count,
    const pitchee_hnr_vad_segment_t* vad_segments,
    size_t vad_segment_count
);

}  // namespace pitchee

#endif

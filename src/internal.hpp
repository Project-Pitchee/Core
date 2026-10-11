#ifndef PITCHEE_INTERNAL_HPP
#define PITCHEE_INTERNAL_HPP

#include <cstdint>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace pitchee {

constexpr int kSampleRate = 16000;
constexpr int kPatchSamples = 24240;
constexpr int kStrideSamples = 1600;
constexpr int kEmbeddingBatchSize = 8;
constexpr int kEmbeddingDimensions = 192;
constexpr int kSchemaVersion = 4;
// An infinite limit disables input truncation in the analyzer.
constexpr double kMaximumSeconds = std::numeric_limits<double>::infinity();

struct VadSegment {
    double source_start_seconds = 0.0;
    double source_end_seconds = 0.0;
    double speech_start_seconds = 0.0;
    double speech_end_seconds = 0.0;
};

struct VadResult {
    std::vector<VadSegment> segments;
    double input_seconds = 0.0;
    double speech_seconds = 0.0;
    int silero_segment_count = 0;
    int discarded_breath_like_count = 0;
    int trimmed_segment_count = 0;
};

struct CompositeScore {
    double base_score = 0.0;
    double final_score = 0.0;
    bool has_score_cap = false;
    double score_cap = 0.0;
    bool score_limited = false;
    bool score_boosted = false;
    std::string score_rule;
};

struct VfpWindow {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    double vfp_standard_score = 0.0;
};

struct NaturalnessWindow {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    double score = 0.0;
};

struct F0Window {
    double start_seconds = 0.0;
    double end_seconds = 0.0;
    bool has_f0 = false;
    double f0_hz = 0.0;
};

struct SampleWindow {
    size_t start = 0;
    size_t length = 0;
};

struct VoiceHealthResult {
    double disorder_probability = 0.0;
    double health_score = 0.0;
    bool disordered = false;
    std::array<double, 14> acoustic{};
    std::vector<double> ecapa_features;
};

struct AnalysisResult {
    std::string model_version = "unknown";
    int score_profile = 0;
    double vfp_standard_score = 0.0;
    int window_count = 0;
    std::vector<VfpWindow> vfp_windows;
    double window_duration_seconds = 0.0;
    std::vector<NaturalnessWindow> naturalness_windows;
    double naturalness_window_duration_seconds = 0.0;
    int source_sample_rate = 0;
    int source_channels = 0;
    double source_seconds = 0.0;
    double analyzed_seconds = 0.0;
    bool has_f0 = false;
    double f0_mean_hz = 0.0;
    double f0_standard_deviation_hz = 0.0;
    int voiced_frame_count = 0;
    int voiced_window_count = 0;
    std::vector<F0Window> f0_windows;
    double naturalness_score = 0.0;
    VoiceHealthResult voice_health;
    CompositeScore score;
    VadResult vad;
};

struct PitchResult {
    std::vector<float> pitch_hz;
    std::vector<float> confidence;
    std::vector<float> timestamps;
    std::vector<uint8_t> voicing;
    std::vector<F0Window> windows;
    bool has_mean_f0 = false;
    double mean_f0_hz = 0.0;
    double standard_deviation_f0_hz = 0.0;
    int voiced_frame_count = 0;
    int voiced_window_count = 0;
};

class ModelRuntime;

class NaturalnessModel {
public:
    NaturalnessModel(
        const std::filesystem::path& path,
        int intra_op_threads
    );
    ~NaturalnessModel();
    double score(const std::vector<float>& features) const;

private:
    std::unique_ptr<ModelRuntime> model_;
};

CompositeScore calculate_composite_score(
    int score_profile,
    double vfp_standard_score,
    double naturalness_score,
    bool has_f0,
    double f0_hz
);

std::string result_to_json(const AnalysisResult& result);
std::string escape_json(const std::string& value);
std::string model_version_from_directory(const std::filesystem::path& directory);

std::vector<float> resample_mono(
    const float* interleaved,
    size_t sample_count,
    int channels,
    int source_rate,
    int target_rate = kSampleRate
);

std::vector<SampleWindow> native_speech_windows(
    size_t sample_count,
    const std::vector<VadSegment>& segments,
    size_t stride_samples = kStrideSamples
);

std::vector<float> crop_window(
    const std::vector<float>& signal,
    const SampleWindow& window
);

void normalize_l2(float* values, size_t size);

}  // namespace pitchee

#endif

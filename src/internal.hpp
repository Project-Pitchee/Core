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
constexpr int kSchemaVersion = 3;
// An infinite limit disables input truncation in the analyzer.
constexpr double kMaximumSeconds = std::numeric_limits<double>::infinity();

struct VadSegment {
    double source_start_seconds = 0.0;
    double source_end_seconds = 0.0;
    double speech_start_seconds = 0.0;
    double speech_end_seconds = 0.0;
};

// Why a clip ended up with no usable speech. The gates are independent:
// Silero may yield no raw segment at all, every raw segment may fail the
// periodicity filter that removes breath-like audio, or what survives may be
// too short to estimate perturbation features from.
enum class VadRejection {
    kNone = 0,
    kSileroProbability = 1,
    kPeriodicityBreathLike = 2,
    kInsufficientSpeech = 3,
};

// Voice-activity operating point. The defaults are the product-wide general
// values and must not be loosened: recording, realtime feedback and the
// scoring path all run on them. The health-scoring path uses its own looser
// operating point (voice_health_vad_thresholds) because severe breathy
// dysphonia has little periodic energy and the general periodicity gate
// discards it as breath. See docs/vad_health_gate.md.
struct VadThresholds {
    double speech_start_probability = 0.60;
    double speech_end_probability = 0.30;
    double minimum_periodicity = 0.50;
    double minimum_voiced_ratio = 0.12;
    // 0 keeps the historical behaviour: any surviving segment counts as
    // speech, however short. Only the health profile sets a floor.
    double minimum_speech_seconds = 0.0;
};

inline VadThresholds general_vad_thresholds() { return VadThresholds{}; }

// Operating point for the vocal-health scoring path only (pain point 13).
//
// Measured on the 7 PVQD clips the general operating point rejects
// (vad_eval/ in the VocalHealth-v2 repo; raw grids in vad_eval/sweep_*.csv):
//
//   * 6 of those 7 are rejected by the periodicity gate, not by Silero.
//     Relaxing minimum_periodicity 0.50 -> 0.30 and minimum_voiced_ratio
//     0.12 -> 0.08 recovers them. Severe PT118 then retains 3.83 s of speech
//     and PT136 11.98 s; the healthy reference clips move by under 0.2 s.
//   * Silero's own thresholds stay at the general values. Dropping them to
//     0.25/0.12 buys only 0.12 s on the one Silero-rejected clip (PT084) and
//     Silero is the gate that holds back silence, broadband noise, mains hum
//     and breath-like noise, so it is the wrong gate to loosen.
//   * minimum_speech_seconds exists because the perturbation estimators in
//     voice_health.cpp return 0.0 instead of failing when they have too few
//     frames. Without a floor, a clip that retains 0.09 s of speech is scored
//     from zero-filled jitter/shimmer/CPPS: a fabricated score. 2.0 s keeps
//     PT118 (3.83 s) with margin and correctly refuses PT084 (<= 0.12 s) and
//     PT087 (<= 0.55 s), which carry no measurable voiced speech at any
//     threshold in the grid.
//
// Changing these four numbers changes who gets a health score. Re-run
// vad_eval/run_probe.sh and update the numbers above before editing them.
inline VadThresholds voice_health_vad_thresholds() {
    VadThresholds thresholds;
    thresholds.minimum_periodicity = 0.30;
    thresholds.minimum_voiced_ratio = 0.08;
    thresholds.minimum_speech_seconds = 2.0;
    return thresholds;
}

struct VadResult {
    std::vector<VadSegment> segments;
    double input_seconds = 0.0;
    double speech_seconds = 0.0;
    int silero_segment_count = 0;
    int discarded_breath_like_count = 0;
    int trimmed_segment_count = 0;
    VadRejection rejection = VadRejection::kNone;
    VadThresholds thresholds{};
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

// Whether a clip carried speech the health features can actually be measured
// on. Mirrors pitchee_voice_health_assessment_t.
struct VoiceHealthAssessment {
    bool assessable = false;
    VadRejection rejection = VadRejection::kNone;
    double input_seconds = 0.0;
    double speech_seconds = 0.0;
    int silero_segment_count = 0;
    int discarded_breath_like_count = 0;
};

struct VoiceHealthResult {
    double disorder_probability = 0.0;
    double health_score = 0.0;
    bool disordered = false;
    std::array<double, 14> acoustic{};
    std::vector<double> ecapa_features;
    double grbas_grade = 0.0;
    double cape_v_severity = 0.0;
    VoiceHealthAssessment assessment;
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

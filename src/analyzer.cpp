#include "internal.hpp"
#include "model_runtime.hpp"
#include "vad.hpp"
#include "voice_health.hpp"
#include "voice_health_model.hpp"
#include "wav_reader.hpp"

#include "pitchee/pitchee.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>

struct pitchee_analyzer_t {
    std::filesystem::path model_directory;
    std::string model_version = "unknown";
    int intra_op_threads = 2;
    bool use_coreml = true;
    std::unique_ptr<pitchee::ModelRuntime> ecapa_frontend;
    std::unique_ptr<pitchee::ModelRuntime> ecapa;
    std::unique_ptr<pitchee::ModelRuntime> vfp_head;
    std::unique_ptr<pitchee::ModelRuntime> swift_f0;
    std::unique_ptr<pitchee::VadDetector> vad;
    std::unique_ptr<pitchee::NaturalnessModel> naturalness;
    float min_f0_hz = 75.0f;
    float max_f0_hz = 600.0f;
    float min_f0_confidence = 0.9f;
};

struct pitchee_realtime_f0_t {
    pitchee::ModelRuntime* model = nullptr;
    size_t context_samples = 5120;
    size_t inference_hop_samples = 256;
    size_t buffer_start_sample = 0;
    size_t total_samples = 0;
    size_t samples_since_inference = 0;
    bool has_emitted = false;
    int64_t last_emitted_center_sample = -1;
    int64_t first_emitted_center_sample = -1;
    double output_rate_hz = 0.0;
    double output_interval_samples = 0.0;
    double next_output_sample = 0.0;
    double start_timestamp_seconds = 0.0;
    float min_f0_hz = 75.0f;
    float max_f0_hz = 600.0f;
    float min_f0_confidence = 0.9f;
    std::vector<float> buffer;
    uint64_t process_calls = 0;
    uint64_t inference_calls = 0;
    uint64_t emitted_frames = 0;
    double total_preprocess_ms = 0.0;
    double total_model_ms = 0.0;
    double total_postprocess_ms = 0.0;
    double total_ms = 0.0;
    double last_preprocess_ms = 0.0;
    double last_model_ms = 0.0;
    double last_postprocess_ms = 0.0;
    double last_total_ms = 0.0;
};

namespace {

constexpr double kF0WindowSeconds = 0.05;
constexpr int kSwiftF0FrameHopSamples = 256;
constexpr int kRealtimeF0MinContextSamples = 1024;
constexpr double kSwiftF0FrameIntervalSeconds =
    static_cast<double>(kSwiftF0FrameHopSamples) / pitchee::kSampleRate;
constexpr double kSwiftF0ModelOutputRateHz =
    static_cast<double>(pitchee::kSampleRate) / kSwiftF0FrameHopSamples;

struct PitchTiming {
    double model_ms = 0.0;
};

struct F0Thresholds {
    float min_hz = 75.0f;
    float max_hz = 600.0f;
    float min_confidence = 0.9f;
};

F0Thresholds normalize_f0_thresholds(
    float min_f0_hz,
    float max_f0_hz,
    float min_confidence
) {
    F0Thresholds result;
    result.min_hz = min_f0_hz > 0.0f ? min_f0_hz : 75.0f;
    result.max_hz = max_f0_hz > 0.0f ? max_f0_hz : 600.0f;
    result.min_confidence = min_confidence > 0.0f ? min_confidence : 0.9f;
    if (!std::isfinite(result.min_hz)
        || !std::isfinite(result.max_hz)
        || !std::isfinite(result.min_confidence)
        || result.min_hz >= result.max_hz
        || result.min_confidence > 1.0f) {
        throw std::invalid_argument("invalid SwiftF0 thresholds");
    }
    return result;
}

bool valid_score_profile(pitchee_score_profile_t score_profile) {
    return score_profile == PITCHEE_SCORE_PROFILE_FEMINIZATION
        || score_profile == PITCHEE_SCORE_PROFILE_MASCULINIZATION;
}

struct ProgressReporter {
    pitchee_progress_callback_t progress_callback = nullptr;
    void* progress_user_data = nullptr;

    void progress(
        pitchee_progress_stage_t stage,
        uint64_t completed,
        uint64_t total
    ) const {
        if (!progress_callback) return;
        pitchee_progress_t progress{};
        progress.stage = stage;
        progress.completed = std::min(completed, total);
        progress.total = total;
        progress.fraction = total > 0
            ? static_cast<double>(progress.completed) / total
            : (stage == PITCHEE_PROGRESS_STAGE_COMPLETED ? 1.0 : 0.0);
        progress_callback(&progress, progress_user_data);
    }
};

void set_error(char* target, size_t capacity, const std::string& message) {
    if (!target || capacity == 0) return;
    const size_t count = std::min(capacity - 1, message.size());
    std::memcpy(target, message.data(), count);
    target[count] = '\0';
}

pitchee_status_t status_for_exception(const std::exception& error) {
    const std::string message = error.what();
    if (message.find("without ONNX Runtime") != std::string::npos) {
        return PITCHEE_ERROR_ORT_UNAVAILABLE;
    }
    if (message.find("Silero VAD detected no speech") != std::string::npos) {
        return PITCHEE_ERROR_NO_SPEECH;
    }
    if (message.find("unsupported WAV") != std::string::npos) {
        return PITCHEE_ERROR_UNSUPPORTED_FORMAT;
    }
    if (message.find("WAV") != std::string::npos
        || message.find("unable to open") != std::string::npos) {
        return PITCHEE_ERROR_IO;
    }
    if (message.find("model") != std::string::npos
        || message.find("ONNX") != std::string::npos) {
        return PITCHEE_ERROR_MODEL;
    }
    return PITCHEE_ERROR_INTERNAL;
}

pitchee::PitchResult analyze_pitch(
    pitchee::ModelRuntime& model,
    const std::vector<float>& samples,
    const ProgressReporter& reporter,
    const F0Thresholds& thresholds,
    PitchTiming* timing = nullptr
) {
    reporter.progress(PITCHEE_PROGRESS_STAGE_ANALYZING_F0, 0, 1);
    std::vector<float> input = samples;
    if (input.size() < 256) input.resize(256, 0.0f);
    pitchee::Tensor tensor;
    tensor.shape = {1, static_cast<int64_t>(input.size())};
    tensor.values = std::move(input);
    const auto model_started = std::chrono::steady_clock::now();
    auto outputs = model.run_all({{"input_audio", std::move(tensor)}});
    if (timing) {
        timing->model_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - model_started
        ).count();
    }
    if (outputs.size() < 2 || outputs[0].values.size() != outputs[1].values.size()) {
        throw std::runtime_error("SwiftF0 returned invalid output");
    }

    pitchee::PitchResult result;
    result.pitch_hz = std::move(outputs[0].values);
    result.confidence = std::move(outputs[1].values);
    result.timestamps.resize(result.pitch_hz.size());
    result.voicing.resize(result.pitch_hz.size(), 0);
    std::vector<float> voiced_pitch;
    const size_t f0_window_samples = static_cast<size_t>(
        kF0WindowSeconds * pitchee::kSampleRate
    );
    const size_t f0_window_count = std::max<size_t>(
        1,
        (samples.size() + f0_window_samples - 1) / f0_window_samples
    );
    std::vector<double> f0_window_sums(f0_window_count, 0.0);
    std::vector<size_t> f0_window_counts(f0_window_count, 0);
    for (size_t index = 0; index < result.pitch_hz.size(); ++index) {
        result.timestamps[index] = static_cast<float>(
            (static_cast<double>(index * 256) + 127.5) / 16000.0
        );
        const bool voiced =
            result.confidence[index] > thresholds.min_confidence
            && result.pitch_hz[index] >= thresholds.min_hz
            && result.pitch_hz[index] <= thresholds.max_hz;
        result.voicing[index] = voiced ? 1 : 0;
        if (voiced) {
            voiced_pitch.push_back(result.pitch_hz[index]);
            const size_t window_index = std::min(
                f0_window_count - 1,
                static_cast<size_t>(
                    result.timestamps[index] / kF0WindowSeconds
                )
            );
            f0_window_sums[window_index] += result.pitch_hz[index];
            ++f0_window_counts[window_index];
        }
    }

    const double total_seconds = static_cast<double>(samples.size())
        / pitchee::kSampleRate;
    result.windows.reserve(f0_window_count);
    for (size_t index = 0; index < f0_window_count; ++index) {
        pitchee::F0Window window;
        window.start_seconds = index * kF0WindowSeconds;
        window.end_seconds = std::min(
            total_seconds,
            (index + 1) * kF0WindowSeconds
        );
        if (f0_window_counts[index] > 0) {
            window.has_f0 = true;
            window.f0_hz = f0_window_sums[index]
                / static_cast<double>(f0_window_counts[index]);
            ++result.voiced_window_count;
        }
        result.windows.push_back(window);
    }

    if (!voiced_pitch.empty()) {
        const double mean = std::accumulate(
            voiced_pitch.begin(),
            voiced_pitch.end(),
            0.0
        ) / voiced_pitch.size();
        double variance = 0.0;
        for (const float value : voiced_pitch) {
            const double delta = value - mean;
            variance += delta * delta;
        }
        result.has_mean_f0 = true;
        result.mean_f0_hz = mean;
        result.standard_deviation_f0_hz = std::sqrt(variance / voiced_pitch.size());
        result.voiced_frame_count = static_cast<int>(voiced_pitch.size());
    }
    reporter.progress(PITCHEE_PROGRESS_STAGE_ANALYZING_F0, 1, 1);
    return result;
}



pitchee::Tensor canonicalize_ecapa_features(pitchee::Tensor features);

std::vector<std::vector<float>> embed_waveforms(
    pitchee::ModelRuntime& frontend,
    pitchee::ModelRuntime& encoder,
    const std::vector<std::vector<float>>& waveforms,
    const ProgressReporter& reporter,
    pitchee_progress_stage_t stage
) {
    std::vector<std::vector<float>> embeddings(waveforms.size());
    std::map<size_t, std::vector<size_t>> groups;
    for (size_t index = 0; index < waveforms.size(); ++index) {
        if (waveforms[index].empty()) continue;
        groups[waveforms[index].size()].push_back(index);
    }
    if (groups.empty()) throw std::runtime_error("no waveform windows");

    reporter.progress(stage, 0, waveforms.size());
    size_t completed = 0;
    for (const auto& [window_samples, indices] : groups) {
        for (size_t batch_start = 0;
             batch_start < indices.size();
             batch_start += pitchee::kEmbeddingBatchSize) {
            const size_t real_count = std::min<size_t>(
                pitchee::kEmbeddingBatchSize,
                indices.size() - batch_start
            );
            pitchee::Tensor input;
            input.shape = {
                pitchee::kEmbeddingBatchSize,
                static_cast<int64_t>(window_samples),
            };
            input.values.reserve(
                static_cast<size_t>(pitchee::kEmbeddingBatchSize)
                * window_samples
            );
            for (size_t index = 0; index < pitchee::kEmbeddingBatchSize; ++index) {
                if (index < real_count) {
                    const auto& waveform = waveforms[indices[batch_start + index]];
                    input.values.insert(
                        input.values.end(),
                        waveform.begin(),
                        waveform.end()
                    );
                } else {
                    input.values.insert(input.values.end(), window_samples, 0.0f);
                }
            }

            auto features = frontend.run({{"waveforms", std::move(input)}});
            features = canonicalize_ecapa_features(std::move(features));
            auto batch_embeddings = encoder.run(
                {{"features", std::move(features)}}
            );
            if (batch_embeddings.values.size()
                < static_cast<size_t>(pitchee::kEmbeddingBatchSize)
                    * pitchee::kEmbeddingDimensions) {
                throw std::runtime_error("ECAPA returned invalid embeddings");
            }
            for (size_t index = 0; index < real_count; ++index) {
                const auto start = batch_embeddings.values.begin()
                    + static_cast<std::ptrdiff_t>(
                        index * pitchee::kEmbeddingDimensions
                    );
                embeddings[indices[batch_start + index]].assign(
                    start,
                    start + pitchee::kEmbeddingDimensions
                );
            }
            completed += real_count;
            reporter.progress(stage, completed, waveforms.size());
        }
    }
    return embeddings;
}

pitchee::Tensor canonicalize_ecapa_features(pitchee::Tensor features) {
    constexpr int64_t target_frames = 150;
    constexpr int64_t feature_count = 80;
    if (features.shape.size() != 3
        || features.shape[0] <= 0
        || features.shape[1] == target_frames
        || features.shape[2] != feature_count) {
        return features;
    }

    const int64_t batch = features.shape[0];
    const int64_t source_frames = features.shape[1];
    const int64_t copy_frames = std::min(source_frames, target_frames);
    // The ncnn ECAPA export uses a fixed-width attention mask. Keep every
    // batch at the same frame count so short tails pad and long tails crop.
    pitchee::Tensor resized;
    resized.shape = {batch, target_frames, feature_count};
    resized.values.assign(
        static_cast<size_t>(batch * target_frames * feature_count),
        0.0f
    );
    for (int64_t batch_index = 0; batch_index < batch; ++batch_index) {
        const auto source_start = features.values.begin()
            + static_cast<std::ptrdiff_t>(
                batch_index * source_frames * feature_count
            );
        auto target_start = resized.values.begin()
            + static_cast<std::ptrdiff_t>(
                batch_index * target_frames * feature_count
            );
        std::copy(
            source_start,
            source_start + static_cast<std::ptrdiff_t>(copy_frames * feature_count),
            target_start
        );
    }
    return resized;
}

std::vector<double> classify_embeddings(
    pitchee::ModelRuntime& model,
    const std::vector<std::vector<float>>& embeddings,
    const ProgressReporter& reporter
) {
    std::vector<double> probabilities;
    probabilities.reserve(embeddings.size());
    reporter.progress(
        PITCHEE_PROGRESS_STAGE_CLASSIFYING_VFP_WINDOWS,
        0,
        embeddings.size()
    );
    for (size_t batch_start = 0;
         batch_start < embeddings.size();
         batch_start += pitchee::kEmbeddingBatchSize) {
        const size_t real_count = std::min<size_t>(
            pitchee::kEmbeddingBatchSize,
            embeddings.size() - batch_start
        );
        pitchee::Tensor input;
        input.shape = {
            pitchee::kEmbeddingBatchSize,
            pitchee::kEmbeddingDimensions,
        };
        input.values.reserve(
            static_cast<size_t>(pitchee::kEmbeddingBatchSize)
            * pitchee::kEmbeddingDimensions
        );
        for (size_t index = 0; index < pitchee::kEmbeddingBatchSize; ++index) {
            if (index < real_count) {
                input.values.insert(
                    input.values.end(),
                    embeddings[batch_start + index].begin(),
                    embeddings[batch_start + index].end()
                );
            } else {
                input.values.insert(
                    input.values.end(),
                    pitchee::kEmbeddingDimensions,
                    0.0f
                );
            }
        }
        auto output = model.run({{"embeddings", std::move(input)}});
        if (output.values.size() < real_count) {
            throw std::runtime_error("VFP classifier returned invalid output");
        }
        for (size_t index = 0; index < real_count; ++index) {
            probabilities.push_back(output.values[index]);
        }
        reporter.progress(
            PITCHEE_PROGRESS_STAGE_CLASSIFYING_VFP_WINDOWS,
            std::min(
                embeddings.size(),
                batch_start + pitchee::kEmbeddingBatchSize
            ),
            embeddings.size()
        );
    }
    return probabilities;
}

std::vector<float> naturalness_features(
    const std::vector<std::vector<float>>& embeddings
) {
    if (embeddings.empty()) throw std::invalid_argument("no naturalness embeddings");
    std::vector<std::vector<float>> normalized = embeddings;
    for (auto& embedding : normalized) {
        pitchee::normalize_l2(embedding.data(), embedding.size());
    }

    std::vector<float> raw_mean(pitchee::kEmbeddingDimensions, 0.0f);
    for (const auto& embedding : normalized) {
        for (size_t index = 0; index < raw_mean.size(); ++index) {
            raw_mean[index] += embedding[index];
        }
    }
    for (float& value : raw_mean) value /= normalized.size();

    std::vector<float> mean = raw_mean;
    pitchee::normalize_l2(mean.data(), mean.size());
    std::vector<double> variance(pitchee::kEmbeddingDimensions, 0.0);
    for (const auto& embedding : normalized) {
        for (size_t index = 0; index < variance.size(); ++index) {
            const double delta = static_cast<double>(embedding[index]) - raw_mean[index];
            variance[index] += delta * delta;
        }
    }
    std::vector<float> features = mean;
    features.reserve(pitchee::kEmbeddingDimensions * 2);
    for (double value : variance) {
        features.push_back(static_cast<float>(
            std::sqrt(value / normalized.size())
        ));
    }
    return features;
}

}  // namespace

extern "C" {

const char* pitchee_version(void) {
    return "0.5.0";
}

pitchee_status_t pitchee_analyzer_create(
    const char* model_directory,
    int32_t intra_op_threads,
    int32_t use_coreml,
    float min_f0_hz,
    float max_f0_hz,
    float min_confidence,
    pitchee_analyzer_t** out_analyzer,
    char* error_message,
    size_t error_message_capacity
) {
    if (!model_directory || !out_analyzer) {
        set_error(error_message, error_message_capacity, "invalid argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    try {
        auto analyzer = std::make_unique<pitchee_analyzer_t>();
        analyzer->model_directory = model_directory;
        analyzer->model_version = pitchee::model_version_from_directory(
            analyzer->model_directory
        );
        analyzer->intra_op_threads = intra_op_threads > 0
            ? intra_op_threads
            : 2;
        analyzer->use_coreml = use_coreml != 0;
        const F0Thresholds thresholds = normalize_f0_thresholds(
            min_f0_hz,
            max_f0_hz,
            min_confidence
        );
        analyzer->min_f0_hz = thresholds.min_hz;
        analyzer->max_f0_hz = thresholds.max_hz;
        analyzer->min_f0_confidence = thresholds.min_confidence;
        const auto directory = analyzer->model_directory;
        analyzer->ecapa_frontend = std::make_unique<pitchee::ModelRuntime>(
            directory / "ECAPAFrontend.onnx",
            analyzer->intra_op_threads,
            false
        );
        analyzer->ecapa = std::make_unique<pitchee::ModelRuntime>(
            directory / "ECAPA.onnx",
            analyzer->intra_op_threads,
            analyzer->use_coreml
        );
        analyzer->vfp_head = std::make_unique<pitchee::ModelRuntime>(
            directory / "VFPHead.onnx",
            analyzer->intra_op_threads,
            false
        );
        analyzer->swift_f0 = std::make_unique<pitchee::ModelRuntime>(
            directory / "SwiftF0.onnx",
            analyzer->intra_op_threads,
            false
        );
        analyzer->vad = std::make_unique<pitchee::VadDetector>(
            directory / "SileroVAD.onnx",
            analyzer->intra_op_threads
        );
        analyzer->naturalness = std::make_unique<pitchee::NaturalnessModel>(
            directory / "Naturalness.onnx",
            analyzer->intra_op_threads
        );
        *out_analyzer = analyzer.release();
        return PITCHEE_SUCCESS;
    } catch (const std::invalid_argument& error) {
        set_error(error_message, error_message_capacity, error.what());
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

void pitchee_analyzer_destroy(pitchee_analyzer_t* analyzer) {
    delete analyzer;
}

pitchee_status_t pitchee_analyzer_detect_vad(
    pitchee_analyzer_t* analyzer,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    pitchee_vad_segment_callback_t segment_callback,
    void* user_data,
    char* error_message,
    size_t error_message_capacity
) {
    if (!analyzer || !samples || sample_count == 0 || sample_rate <= 0
        || channels <= 0) {
        set_error(error_message, error_message_capacity, "invalid VAD argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    try {
        const std::vector<float> signal = pitchee::resample_mono(
            samples,
            sample_count,
            channels,
            sample_rate,
            pitchee::kSampleRate
        );
        if (signal.empty()) throw std::invalid_argument("empty audio");
        const auto vad = analyzer->vad->detect(signal);
        if (segment_callback) {
            for (const auto& segment : vad.segments) {
                const pitchee_vad_segment_t output{
                    segment.source_start_seconds,
                    segment.source_end_seconds,
                };
                segment_callback(&output, user_data);
            }
        }
        return PITCHEE_SUCCESS;
    } catch (const std::invalid_argument& error) {
        set_error(error_message, error_message_capacity, error.what());
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

pitchee_status_t run_voice_health_pcm_impl(
    pitchee_analyzer_t* analyzer,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    const ProgressReporter& reporter,
    pitchee_voice_health_result_t* out_result,
    char* error_message,
    size_t error_message_capacity
) {
    if (!analyzer || !samples || sample_count == 0 || sample_rate <= 0
        || channels <= 0 || !out_result) {
        set_error(error_message, error_message_capacity, "invalid voice health PCM argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    *out_result = {};
    try {
        reporter.progress(PITCHEE_PROGRESS_STAGE_RESAMPLING_AUDIO, 0, 1);
        const std::vector<float> signal = pitchee::resample_mono(
            samples,
            sample_count,
            channels,
            sample_rate,
            pitchee::kSampleRate
        );
        if (signal.empty()) throw std::invalid_argument("empty audio");
        reporter.progress(PITCHEE_PROGRESS_STAGE_RESAMPLING_AUDIO, 1, 1);

        const F0Thresholds f0_thresholds{
            analyzer->min_f0_hz,
            analyzer->max_f0_hz,
            analyzer->min_f0_confidence,
        };
        const auto pitch = analyze_pitch(
            *analyzer->swift_f0,
            signal,
            reporter,
            f0_thresholds
        );
        const auto vad = analyzer->vad->detect(
            signal,
            [&reporter](size_t completed, size_t total) {
                reporter.progress(
                    PITCHEE_PROGRESS_STAGE_DETECTING_SPEECH,
                    completed,
                    total
                );
            }
        );
        if (vad.segments.empty()) {
            throw std::runtime_error("Silero VAD detected no speech");
        }

        reporter.progress(PITCHEE_PROGRESS_STAGE_PREPARING_VOICE_HEALTH, 0, 1);
        const auto windows = pitchee::voice_health_windows(signal.size());
        std::vector<std::vector<float>> patches;
        patches.reserve(windows.size());
        for (const auto& window : windows) {
            patches.push_back(pitchee::crop_window(signal, window));
        }
        reporter.progress(PITCHEE_PROGRESS_STAGE_PREPARING_VOICE_HEALTH, 1, 1);

        const auto embeddings = embed_waveforms(
            *analyzer->ecapa_frontend,
            *analyzer->ecapa,
            patches,
            reporter,
            PITCHEE_PROGRESS_STAGE_EXTRACTING_VOICE_HEALTH_EMBEDDINGS
        );
        reporter.progress(PITCHEE_PROGRESS_STAGE_SCORING_VOICE_HEALTH, 0, 1);
        const auto health = pitchee::analyze_voice_health(
            signal,
            embeddings,
            pitch,
            vad
        );
        reporter.progress(PITCHEE_PROGRESS_STAGE_SCORING_VOICE_HEALTH, 1, 1);

        out_result->health_score = health.health_score;
        out_result->disorder_probability = health.disorder_probability;
        out_result->decision_threshold = pitchee::kVoiceHealthDecisionThreshold;
        out_result->is_disordered = health.disordered ? 1 : 0;
        out_result->features = {
            health.acoustic[0],
            health.acoustic[1],
            health.acoustic[2],
            health.acoustic[3],
            health.acoustic[4],
            health.acoustic[5],
            health.acoustic[6],
            health.acoustic[7],
            health.acoustic[8],
            health.acoustic[9],
            health.acoustic[10],
            health.acoustic[11],
            health.acoustic[12],
            health.acoustic[13],
        };
        reporter.progress(PITCHEE_PROGRESS_STAGE_COMPLETED, 1, 1);
        return PITCHEE_SUCCESS;
    } catch (const std::invalid_argument& error) {
        set_error(error_message, error_message_capacity, error.what());
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

pitchee_status_t analyze_pcm_impl(
    pitchee_analyzer_t* analyzer,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    pitchee_score_profile_t score_profile,
    const ProgressReporter& reporter,
    char** out_json,
    char* error_message,
    size_t error_message_capacity
) {
    if (!analyzer || !samples || sample_count == 0 || sample_rate <= 0
        || channels <= 0 || !valid_score_profile(score_profile) || !out_json) {
        set_error(error_message, error_message_capacity, "invalid PCM argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    *out_json = nullptr;
    try {
        reporter.progress(PITCHEE_PROGRESS_STAGE_LOADING_AUDIO, 1, 1);
        reporter.progress(PITCHEE_PROGRESS_STAGE_RESAMPLING_AUDIO, 0, 1);
        std::vector<float> signal = pitchee::resample_mono(
            samples,
            sample_count,
            channels,
            sample_rate,
            pitchee::kSampleRate
        );
        if (std::isfinite(pitchee::kMaximumSeconds)) {
            const size_t maximum_samples = static_cast<size_t>(
                pitchee::kMaximumSeconds * pitchee::kSampleRate
            );
            if (signal.size() > maximum_samples) signal.resize(maximum_samples);
        }
        if (signal.empty()) throw std::invalid_argument("empty audio");
        const double source_seconds =
            static_cast<double>(signal.size()) / pitchee::kSampleRate;
        reporter.progress(PITCHEE_PROGRESS_STAGE_RESAMPLING_AUDIO, 1, 1);

        const F0Thresholds f0_thresholds{
            analyzer->min_f0_hz,
            analyzer->max_f0_hz,
            analyzer->min_f0_confidence,
        };
        const auto pitch = analyze_pitch(
            *analyzer->swift_f0,
            signal,
            reporter,
            f0_thresholds
        );
        auto vad = analyzer->vad->detect(
            signal,
            [&reporter](size_t completed, size_t total) {
                reporter.progress(
                    PITCHEE_PROGRESS_STAGE_DETECTING_SPEECH,
                    completed,
                    total
                );
            }
        );
        if (vad.segments.empty()) {
            throw std::runtime_error("Silero VAD detected no speech");
        }

        reporter.progress(PITCHEE_PROGRESS_STAGE_PREPARING_VFP_WINDOWS, 0, 1);
        const auto source_windows = pitchee::native_speech_windows(
            signal.size(),
            vad.segments
        );
        if (source_windows.empty()) {
            throw std::runtime_error("Silero VAD detected no speech");
        }
        reporter.progress(PITCHEE_PROGRESS_STAGE_PREPARING_VFP_WINDOWS, 1, 1);

        std::vector<std::vector<float>> vfp_patches;
        vfp_patches.reserve(source_windows.size());
        for (const auto& window : source_windows) {
            vfp_patches.push_back(pitchee::crop_window(signal, window));
        }
        const auto speech_embeddings = embed_waveforms(
            *analyzer->ecapa_frontend,
            *analyzer->ecapa,
            vfp_patches,
            reporter,
            PITCHEE_PROGRESS_STAGE_EXTRACTING_VFP_EMBEDDINGS
        );
        const auto probabilities = classify_embeddings(
            *analyzer->vfp_head,
            speech_embeddings,
            reporter
        );
        if (probabilities.empty()) throw std::runtime_error("no VFP windows");

        reporter.progress(
            PITCHEE_PROGRESS_STAGE_PREPARING_NATURALNESS_WINDOWS,
            0,
            1
        );
        // Naturalness uses the exact VFP window set on the source timeline.
        const auto& natural_windows = source_windows;
        reporter.progress(
            PITCHEE_PROGRESS_STAGE_PREPARING_NATURALNESS_WINDOWS,
            1,
            1
        );
        reporter.progress(
            PITCHEE_PROGRESS_STAGE_EXTRACTING_NATURALNESS_EMBEDDINGS,
            0,
            speech_embeddings.size()
        );
        const auto& natural_embeddings = speech_embeddings;
        reporter.progress(
            PITCHEE_PROGRESS_STAGE_EXTRACTING_NATURALNESS_EMBEDDINGS,
            natural_embeddings.size(),
            natural_embeddings.size()
        );
        const auto features = naturalness_features(natural_embeddings);

        pitchee::AnalysisResult result;
        result.model_version = analyzer->model_version;
        result.score_profile = score_profile;
        const double mean_probability = std::accumulate(
            probabilities.begin(),
            probabilities.end(),
            0.0
        ) / probabilities.size();
        result.vfp_standard_score = std::max(
            0.0,
            std::min(100.0, mean_probability * 100.0)
        );
        result.window_count = static_cast<int>(probabilities.size());
        result.window_duration_seconds = static_cast<double>(
            pitchee::kPatchSamples
        ) / pitchee::kSampleRate;
        result.vfp_windows.reserve(source_windows.size());
        for (size_t index = 0; index < source_windows.size(); ++index) {
            const double start_seconds = static_cast<double>(
                source_windows[index].start
            ) / pitchee::kSampleRate;
            const double end_seconds = static_cast<double>(
                source_windows[index].start + source_windows[index].length
            ) / pitchee::kSampleRate;
            pitchee::VfpWindow window;
            window.start_seconds = start_seconds;
            window.end_seconds = end_seconds;
            window.vfp_standard_score = std::max(
                0.0,
                std::min(100.0, probabilities[index] * 100.0)
            );
            result.vfp_windows.push_back(window);
        }

        result.naturalness_window_duration_seconds = static_cast<double>(
            pitchee::kPatchSamples
        ) / pitchee::kSampleRate;
        const std::vector<float> global_naturalness_std(
            features.begin() + pitchee::kEmbeddingDimensions,
            features.end()
        );
        result.naturalness_windows.reserve(natural_windows.size());
        reporter.progress(
            PITCHEE_PROGRESS_STAGE_SCORING_NATURALNESS_WINDOWS,
            0,
            natural_windows.size()
        );
        for (size_t index = 0; index < natural_windows.size(); ++index) {
            const double start_seconds = static_cast<double>(
                natural_windows[index].start
            ) / pitchee::kSampleRate;
            const double end_seconds = static_cast<double>(
                natural_windows[index].start + natural_windows[index].length
            ) / pitchee::kSampleRate;
            std::vector<float> window_features = natural_embeddings[index];
            pitchee::normalize_l2(
                window_features.data(),
                window_features.size()
            );
            window_features.insert(
                window_features.end(),
                global_naturalness_std.begin(),
                global_naturalness_std.end()
            );
            pitchee::NaturalnessWindow window;
            window.start_seconds = start_seconds;
            window.end_seconds = end_seconds;
            window.score = analyzer->naturalness->score(window_features);
            result.naturalness_windows.push_back(window);
            reporter.progress(
                PITCHEE_PROGRESS_STAGE_SCORING_NATURALNESS_WINDOWS,
                index + 1,
                natural_windows.size()
            );
        }

        reporter.progress(PITCHEE_PROGRESS_STAGE_CALCULATING_SCORES, 0, 1);
        result.source_sample_rate = sample_rate;
        result.source_channels = channels;
        result.source_seconds = source_seconds;
        result.analyzed_seconds = static_cast<double>(signal.size()) / pitchee::kSampleRate;
        result.has_f0 = pitch.has_mean_f0;
        result.f0_mean_hz = pitch.mean_f0_hz;
        result.f0_standard_deviation_hz = pitch.standard_deviation_f0_hz;
        result.voiced_frame_count = pitch.voiced_frame_count;
        result.voiced_window_count = pitch.voiced_window_count;
        result.f0_windows = pitch.windows;
        result.naturalness_score = analyzer->naturalness->score(features);
        result.score = pitchee::calculate_composite_score(
            score_profile,
            result.vfp_standard_score,
            result.naturalness_score,
            result.has_f0,
            result.f0_mean_hz
        );
        result.vad = vad;
        reporter.progress(PITCHEE_PROGRESS_STAGE_CALCULATING_SCORES, 1, 1);

        reporter.progress(PITCHEE_PROGRESS_STAGE_SERIALIZING_RESULT, 0, 1);
        const std::string json = pitchee::result_to_json(result);
        auto* output = new char[json.size() + 1];
        std::memcpy(output, json.c_str(), json.size() + 1);
        *out_json = output;
        reporter.progress(PITCHEE_PROGRESS_STAGE_SERIALIZING_RESULT, 1, 1);
        reporter.progress(PITCHEE_PROGRESS_STAGE_COMPLETED, 1, 1);
        return PITCHEE_SUCCESS;
    } catch (const std::invalid_argument& error) {
        set_error(error_message, error_message_capacity, error.what());
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

pitchee_status_t pitchee_analyze_pcm(
    pitchee_analyzer_t* analyzer,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    pitchee_score_profile_t score_profile,
    pitchee_progress_callback_t progress_callback,
    void* user_data,
    char** out_json,
    char* error_message,
    size_t error_message_capacity
) {
    const ProgressReporter reporter{
        progress_callback,
        user_data,
    };
    return analyze_pcm_impl(
        analyzer,
        samples,
        sample_count,
        sample_rate,
        channels,
        score_profile,
        reporter,
        out_json,
        error_message,
        error_message_capacity
    );
}

pitchee_status_t analyze_wav_impl(
    pitchee_analyzer_t* analyzer,
    const char* wav_path,
    pitchee_score_profile_t score_profile,
    const ProgressReporter& reporter,
    char** out_json,
    char* error_message,
    size_t error_message_capacity
) {
    if (!analyzer || !wav_path || !valid_score_profile(score_profile) || !out_json) {
        set_error(error_message, error_message_capacity, "invalid WAV argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    *out_json = nullptr;
    try {
        reporter.progress(PITCHEE_PROGRESS_STAGE_LOADING_AUDIO, 0, 1);
        const pitchee::WavData wav = pitchee::read_wav(wav_path);
        return analyze_pcm_impl(
            analyzer,
            wav.samples.data(),
            wav.samples.size(),
            wav.sample_rate,
            wav.channels,
            score_profile,
            reporter,
            out_json,
            error_message,
            error_message_capacity
        );
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

pitchee_status_t pitchee_analyze_wav_file(
    pitchee_analyzer_t* analyzer,
    const char* wav_path,
    pitchee_score_profile_t score_profile,
    pitchee_progress_callback_t progress_callback,
    void* user_data,
    char** out_json,
    char* error_message,
    size_t error_message_capacity
) {
    const ProgressReporter reporter{
        progress_callback,
        user_data,
    };
    return analyze_wav_impl(
        analyzer,
        wav_path,
        score_profile,
        reporter,
        out_json,
        error_message,
        error_message_capacity
    );
}

pitchee_status_t pitchee_analysis_run_voice_health(
    pitchee_analyzer_t* analyzer,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    pitchee_progress_callback_t progress_callback,
    void* user_data,
    pitchee_voice_health_result_t* out_result,
    char* error_message,
    size_t error_message_capacity
) {
    const ProgressReporter reporter{
        progress_callback,
        user_data,
    };
    return run_voice_health_pcm_impl(
        analyzer,
        samples,
        sample_count,
        sample_rate,
        channels,
        reporter,
        out_result,
        error_message,
        error_message_capacity
    );
}

pitchee_status_t pitchee_analysis_run_voice_health_wav_file(
    pitchee_analyzer_t* analyzer,
    const char* wav_path,
    pitchee_progress_callback_t progress_callback,
    void* user_data,
    pitchee_voice_health_result_t* out_result,
    char* error_message,
    size_t error_message_capacity
) {
    if (!analyzer || !wav_path || !out_result) {
        set_error(error_message, error_message_capacity, "invalid voice health WAV argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    const ProgressReporter reporter{
        progress_callback,
        user_data,
    };
    try {
        reporter.progress(PITCHEE_PROGRESS_STAGE_LOADING_AUDIO, 0, 1);
        const pitchee::WavData wav = pitchee::read_wav(wav_path);
        reporter.progress(PITCHEE_PROGRESS_STAGE_LOADING_AUDIO, 1, 1);
        return run_voice_health_pcm_impl(
            analyzer,
            wav.samples.data(),
            wav.samples.size(),
            wav.sample_rate,
            wav.channels,
            reporter,
            out_result,
            error_message,
            error_message_capacity
        );
    } catch (const std::invalid_argument& error) {
        set_error(error_message, error_message_capacity, error.what());
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

pitchee_status_t pitchee_realtime_f0_create_with_options(
    pitchee_analyzer_t* analyzer,
    const pitchee_realtime_f0_options_t* options,
    pitchee_realtime_f0_t** out_stream,
    char* error_message,
    size_t error_message_capacity
) {
    if (!analyzer || !out_stream) {
        set_error(error_message, error_message_capacity, "invalid realtime F0 argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    if (options
        && (options->struct_size < sizeof(*options)
            || options->version != PITCHEE_REALTIME_F0_OPTIONS_VERSION)) {
        set_error(error_message, error_message_capacity, "unsupported realtime F0 options");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    *out_stream = nullptr;
    const int32_t requested_context_samples = options ? options->context_samples : 0;
    const int32_t requested_hop_samples = options
        ? options->inference_hop_samples
        : 0;
    const double requested_output_rate_hz = options
        ? options->output_rate_hz
        : 0.0;
    const double start_timestamp_seconds = options
        ? options->start_timestamp_seconds
        : 0.0;

    const size_t resolved_context_samples = requested_context_samples > 0
        ? static_cast<size_t>(requested_context_samples)
        : 5120;
    const size_t resolved_hop_samples = requested_hop_samples > 0
        ? static_cast<size_t>(requested_hop_samples)
        : 256;
    if (resolved_context_samples < kRealtimeF0MinContextSamples
        || resolved_hop_samples < 1
        || resolved_hop_samples > resolved_context_samples) {
        set_error(error_message, error_message_capacity, "invalid realtime F0 window options");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(requested_output_rate_hz)
        || !std::isfinite(start_timestamp_seconds)) {
        set_error(error_message, error_message_capacity, "invalid realtime F0 output options");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    const double effective_output_rate_hz = requested_output_rate_hz > 0.0
        ? std::min(requested_output_rate_hz, kSwiftF0ModelOutputRateHz)
        : kSwiftF0ModelOutputRateHz;
    try {
        auto stream = std::make_unique<pitchee_realtime_f0_t>();
        stream->model = analyzer->swift_f0.get();
        stream->context_samples = resolved_context_samples;
        stream->inference_hop_samples = resolved_hop_samples;
        stream->output_rate_hz = effective_output_rate_hz;
        stream->output_interval_samples = static_cast<double>(pitchee::kSampleRate)
            / effective_output_rate_hz;
        stream->start_timestamp_seconds = start_timestamp_seconds;
        stream->min_f0_hz = analyzer->min_f0_hz;
        stream->max_f0_hz = analyzer->max_f0_hz;
        stream->min_f0_confidence = analyzer->min_f0_confidence;
        stream->buffer.reserve(resolved_context_samples);
        *out_stream = stream.release();
        return PITCHEE_SUCCESS;
    } catch (const std::invalid_argument& error) {
        set_error(error_message, error_message_capacity, error.what());
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

pitchee_status_t pitchee_realtime_f0_create(
    pitchee_analyzer_t* analyzer,
    int32_t context_samples,
    int32_t hop_samples,
    pitchee_realtime_f0_t** out_stream,
    char* error_message,
    size_t error_message_capacity
) {
    pitchee_realtime_f0_options_t options{};
    options.struct_size = sizeof(options);
    options.version = PITCHEE_REALTIME_F0_OPTIONS_VERSION;
    options.context_samples = context_samples;
    options.inference_hop_samples = hop_samples;
    options.output_rate_hz = 0.0;
    options.start_timestamp_seconds = 0.0;
    return pitchee_realtime_f0_create_with_options(
        analyzer,
        &options,
        out_stream,
        error_message,
        error_message_capacity
    );
}

pitchee_status_t pitchee_realtime_f0_process(
    pitchee_realtime_f0_t* stream,
    const float* samples,
    size_t sample_count,
    pitchee_f0_frame_callback_t frame_callback,
    void* user_data,
    char* error_message,
    size_t error_message_capacity
) {
    if (!stream || !stream->model || (!samples && sample_count > 0)) {
        set_error(error_message, error_message_capacity, "invalid realtime F0 argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    if (sample_count == 0) return PITCHEE_SUCCESS;

    try {
        const auto process_started = std::chrono::steady_clock::now();
        ++stream->process_calls;
        stream->buffer.insert(
            stream->buffer.end(),
            samples,
            samples + sample_count
        );
        stream->total_samples += sample_count;
        stream->samples_since_inference += sample_count;
        if (stream->buffer.size() > stream->context_samples) {
            const size_t drop = stream->buffer.size() - stream->context_samples;
            stream->buffer.erase(
                stream->buffer.begin(),
                stream->buffer.begin() + static_cast<std::ptrdiff_t>(drop)
            );
            stream->buffer_start_sample += drop;
        }
        const auto preprocess_finished = std::chrono::steady_clock::now();

        if (!stream->has_emitted
            && stream->buffer.size() < stream->context_samples) {
            return PITCHEE_SUCCESS;
        }
        if (stream->has_emitted
            && stream->samples_since_inference < stream->inference_hop_samples) {
            return PITCHEE_SUCCESS;
        }

        const ProgressReporter reporter{};
        const F0Thresholds thresholds{
            stream->min_f0_hz,
            stream->max_f0_hz,
            stream->min_f0_confidence,
        };
        PitchTiming timing;
        const auto pitch = analyze_pitch(
            *stream->model,
            stream->buffer,
            reporter,
            thresholds,
            &timing
        );
        const auto inference_finished = std::chrono::steady_clock::now();
        for (size_t index = 0; index < pitch.pitch_hz.size(); ++index) {
            const int64_t center_sample = static_cast<int64_t>(
                stream->buffer_start_sample
            ) + static_cast<int64_t>(std::llround(
                pitch.timestamps[index] * pitchee::kSampleRate
            ));
            if (stream->has_emitted
                && center_sample <= stream->last_emitted_center_sample) {
                continue;
            }
            const double center_sample_value = static_cast<double>(center_sample);
            bool emit = false;
            if (!stream->has_emitted) {
                emit = true;
                stream->next_output_sample = center_sample_value
                    + stream->output_interval_samples;
            } else if (center_sample_value + 1.0e-9 >= stream->next_output_sample) {
                emit = true;
                do {
                    stream->next_output_sample += stream->output_interval_samples;
                } while (stream->next_output_sample <= center_sample_value + 1.0e-9);
            }
            if (!emit) continue;

            const double timestamp = stream->start_timestamp_seconds
                + center_sample_value / pitchee::kSampleRate;
            const float f0 = pitch.pitch_hz[index];
            const float confidence = pitch.confidence[index];
            const bool voiced = confidence > thresholds.min_confidence
                && f0 >= thresholds.min_hz
                && f0 <= thresholds.max_hz;
            if (frame_callback) {
                pitchee_f0_frame_t frame{};
                frame.timestamp_seconds = timestamp;
                frame.f0_hz = f0;
                frame.confidence = confidence;
                frame.voiced = voiced ? 1 : 0;
                frame_callback(&frame, user_data);
            }
            stream->last_emitted_center_sample = center_sample;
            if (stream->first_emitted_center_sample < 0) {
                stream->first_emitted_center_sample = center_sample;
            }
            stream->has_emitted = true;
            ++stream->emitted_frames;
        }
        const auto process_finished = std::chrono::steady_clock::now();
        stream->last_preprocess_ms = std::chrono::duration<double, std::milli>(
            preprocess_finished - process_started
        ).count();
        stream->last_model_ms = timing.model_ms;
        stream->last_postprocess_ms = std::chrono::duration<double, std::milli>(
            process_finished - inference_finished
        ).count();
        stream->last_total_ms = std::chrono::duration<double, std::milli>(
            process_finished - process_started
        ).count();
        stream->total_preprocess_ms += stream->last_preprocess_ms;
        stream->total_model_ms += stream->last_model_ms;
        stream->total_postprocess_ms += stream->last_postprocess_ms;
        stream->total_ms += stream->last_total_ms;
        ++stream->inference_calls;
        stream->samples_since_inference = 0;
        return PITCHEE_SUCCESS;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

void pitchee_realtime_f0_reset(pitchee_realtime_f0_t* stream) {
    if (!stream) return;
    stream->buffer.clear();
    stream->buffer_start_sample = 0;
    stream->total_samples = 0;
    stream->samples_since_inference = 0;
    stream->has_emitted = false;
    stream->last_emitted_center_sample = -1;
    stream->first_emitted_center_sample = -1;
    stream->next_output_sample = 0.0;
    stream->process_calls = 0;
    stream->inference_calls = 0;
    stream->emitted_frames = 0;
    stream->total_preprocess_ms = 0.0;
    stream->total_model_ms = 0.0;
    stream->total_postprocess_ms = 0.0;
    stream->total_ms = 0.0;
    stream->last_preprocess_ms = 0.0;
    stream->last_model_ms = 0.0;
    stream->last_postprocess_ms = 0.0;
    stream->last_total_ms = 0.0;
}

void pitchee_realtime_f0_destroy(pitchee_realtime_f0_t* stream) {
    delete stream;
}

pitchee_status_t pitchee_realtime_f0_get_metadata(
    const pitchee_realtime_f0_t* stream,
    pitchee_realtime_f0_metadata_t* out_metadata,
    char* error_message,
    size_t error_message_capacity
) {
    if (!stream || !out_metadata
        || out_metadata->struct_size < sizeof(*out_metadata)
        || out_metadata->version != PITCHEE_REALTIME_F0_METADATA_VERSION) {
        set_error(error_message, error_message_capacity, "invalid realtime F0 metadata argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    out_metadata->sample_rate = pitchee::kSampleRate;
    out_metadata->frame_hop_samples = kSwiftF0FrameHopSamples;
    out_metadata->frame_interval_seconds = kSwiftF0FrameIntervalSeconds;
    out_metadata->min_context_samples = kRealtimeF0MinContextSamples;
    out_metadata->model_output_rate_hz = kSwiftF0ModelOutputRateHz;
    out_metadata->effective_output_rate_hz = stream->output_rate_hz;
    return PITCHEE_SUCCESS;
}

pitchee_status_t pitchee_realtime_f0_warmup(
    pitchee_realtime_f0_t* stream,
    char* error_message,
    size_t error_message_capacity
) {
    if (!stream || !stream->model) {
        set_error(error_message, error_message_capacity, "invalid realtime F0 argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    try {
        pitchee::Tensor input;
        input.shape = {1, static_cast<int64_t>(stream->context_samples)};
        input.values.assign(stream->context_samples, 0.0f);
        stream->model->run_all({{"input_audio", std::move(input)}});
        return PITCHEE_SUCCESS;
    } catch (const std::exception& error) {
        set_error(error_message, error_message_capacity, error.what());
        return status_for_exception(error);
    }
}

pitchee_status_t pitchee_realtime_f0_get_stats(
    const pitchee_realtime_f0_t* stream,
    pitchee_realtime_f0_stats_t* out_stats,
    char* error_message,
    size_t error_message_capacity
) {
    if (!stream || !out_stats
        || out_stats->struct_size < sizeof(*out_stats)
        || out_stats->version != PITCHEE_REALTIME_F0_STATS_VERSION) {
        set_error(error_message, error_message_capacity, "invalid realtime F0 stats argument");
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    out_stats->process_calls = stream->process_calls;
    out_stats->inference_calls = stream->inference_calls;
    out_stats->emitted_frames = stream->emitted_frames;
    out_stats->total_ms = stream->total_ms;
    out_stats->last_total_ms = stream->last_total_ms;
    out_stats->average_total_ms = stream->inference_calls > 0
        ? stream->total_ms / static_cast<double>(stream->inference_calls)
        : 0.0;
    out_stats->last_preprocess_ms = stream->last_preprocess_ms;
    out_stats->average_preprocess_ms = stream->inference_calls > 0
        ? stream->total_preprocess_ms / static_cast<double>(stream->inference_calls)
        : 0.0;
    out_stats->last_model_ms = stream->last_model_ms;
    out_stats->average_model_ms = stream->inference_calls > 0
        ? stream->total_model_ms / static_cast<double>(stream->inference_calls)
        : 0.0;
    out_stats->last_postprocess_ms = stream->last_postprocess_ms;
    out_stats->average_postprocess_ms = stream->inference_calls > 0
        ? stream->total_postprocess_ms / static_cast<double>(stream->inference_calls)
        : 0.0;
    const int64_t output_span_samples = stream->last_emitted_center_sample
        - stream->first_emitted_center_sample;
    out_stats->measured_output_hz = stream->emitted_frames > 1
        && output_span_samples > 0
        ? static_cast<double>(stream->emitted_frames - 1)
            * pitchee::kSampleRate / static_cast<double>(output_span_samples)
        : 0.0;
    return PITCHEE_SUCCESS;
}

pitchee_status_t pitchee_score(
    pitchee_score_profile_t score_profile,
    double vfp_standard_score,
    double naturalness_score,
    double f0_hz,
    pitchee_score_result_t* out_score
) {
    if (!valid_score_profile(score_profile) || !out_score) {
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    const auto score = pitchee::calculate_composite_score(
        score_profile,
        vfp_standard_score,
        naturalness_score,
        std::isfinite(f0_hz) && f0_hz > 0.0,
        f0_hz
    );
    out_score->base_score = score.base_score;
    out_score->final_score = score.final_score;
    out_score->has_score_cap = score.has_score_cap ? 1 : 0;
    out_score->score_cap = score.score_cap;
    out_score->score_limited = score.score_limited ? 1 : 0;
    out_score->score_boosted = score.score_boosted ? 1 : 0;
    std::memset(out_score->score_rule, 0, sizeof(out_score->score_rule));
    std::memcpy(
        out_score->score_rule,
        score.score_rule.c_str(),
        std::min(
            sizeof(out_score->score_rule) - 1,
            score.score_rule.size()
        )
    );
    return PITCHEE_SUCCESS;
}

void pitchee_string_free(char* value) {
    delete[] value;
}

}  // extern "C"

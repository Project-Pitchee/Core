#include "vad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace pitchee {
namespace {

constexpr int kWindowSize = 512;
constexpr int kContextSize = 64;
constexpr int kStateSize = 2 * 128;
constexpr int kPeriodicityFrameSamples = 400;
constexpr int kPeriodicityHopSamples = 160;

struct RawSegment {
    int start_sample = 0;
    int end_sample = 0;
};

double frame_periodicity(const float* frame, size_t size) {
    if (size != kPeriodicityFrameSamples) return 0.0;
    double mean = 0.0;
    for (size_t index = 0; index < size; ++index) mean += frame[index];
    mean /= size;

    std::vector<double> centered(size);
    double energy = 0.0;
    for (size_t index = 0; index < size; ++index) {
        centered[index] = frame[index] - mean;
        energy += centered[index] * centered[index];
    }
    if (std::sqrt(energy / size) < 1e-4) return 0.0;

    constexpr double pi = 3.14159265358979323846;
    for (size_t index = 0; index < size; ++index) {
        const double hamming = 0.54 - 0.46 * std::cos(
            2.0 * pi * static_cast<double>(index)
            / static_cast<double>(size - 1)
        );
        centered[index] *= hamming;
    }
    energy = 0.0;
    for (const double value : centered) energy += value * value;
    if (energy <= 1e-10) return 0.0;

    double maximum = -1e30;
    const int first_lag = 32;
    const int last_lag = 213;
    for (int lag = first_lag; lag < last_lag; ++lag) {
        double correlation = 0.0;
        for (size_t index = 0; index + lag < centered.size(); ++index) {
            correlation += centered[index] * centered[index + lag];
        }
        maximum = std::max(maximum, correlation / energy);
    }
    return std::isfinite(maximum) ? maximum : 0.0;
}

VadResult finalize_vad_result(
    const std::vector<float>& samples,
    std::vector<RawSegment> raw_segments,
    const VadThresholds& thresholds
) {
    const int speech_padding_samples = kSampleRate * 20 / 1000;

    for (size_t index = 0; index < raw_segments.size(); ++index) {
        if (index == 0) {
            raw_segments[index].start_sample = std::max(
                0,
                raw_segments[index].start_sample - speech_padding_samples
            );
        }
        if (index + 1 < raw_segments.size()) {
            const int silence_duration = raw_segments[index + 1].start_sample
                - raw_segments[index].end_sample;
            if (silence_duration < 2 * speech_padding_samples) {
                raw_segments[index].end_sample += silence_duration / 2;
                raw_segments[index + 1].start_sample = std::max(
                    0,
                    raw_segments[index + 1].start_sample - silence_duration / 2
                );
            } else {
                raw_segments[index].end_sample = std::min(
                    static_cast<int>(samples.size()),
                    raw_segments[index].end_sample + speech_padding_samples
                );
                raw_segments[index + 1].start_sample = std::max(
                    0,
                    raw_segments[index + 1].start_sample - speech_padding_samples
                );
            }
        } else {
            raw_segments[index].end_sample = std::min(
                static_cast<int>(samples.size()),
                raw_segments[index].end_sample + speech_padding_samples
            );
        }
    }

    VadResult result;
    result.thresholds = thresholds;
    result.input_seconds = static_cast<double>(samples.size()) / kSampleRate;
    result.silero_segment_count = static_cast<int>(raw_segments.size());
    double speech_offset = 0.0;
    for (const auto& raw : raw_segments) {
        const int first = std::max(0, raw.start_sample);
        const int last = std::min(static_cast<int>(samples.size()), raw.end_sample);
        if (last <= first) {
            ++result.discarded_breath_like_count;
            continue;
        }

        std::vector<double> periodicities;
        for (int offset = first; offset + kPeriodicityFrameSamples <= last;
             offset += kPeriodicityHopSamples) {
            periodicities.push_back(
                frame_periodicity(
                    samples.data() + offset,
                    kPeriodicityFrameSamples
                )
            );
        }
        std::vector<int> voiced;
        for (size_t index = 0; index < periodicities.size(); ++index) {
            if (periodicities[index] >= thresholds.minimum_periodicity) {
                voiced.push_back(static_cast<int>(index));
            }
        }
        const double voiced_ratio = static_cast<double>(voiced.size())
            / std::max<size_t>(1, periodicities.size());
        if (voiced.empty() || voiced_ratio < thresholds.minimum_voiced_ratio) {
            ++result.discarded_breath_like_count;
            continue;
        }

        const int trimmed_first = first + std::max(
            0,
            voiced.front() * kPeriodicityHopSamples - 80
        );
        const int trimmed_last = first + std::min(
            last - first,
            (voiced.back() + 1) * kPeriodicityHopSamples + 80
        );
        if (trimmed_first > first || trimmed_last < last) {
            ++result.trimmed_segment_count;
        }
        const double duration = static_cast<double>(trimmed_last - trimmed_first)
            / kSampleRate;
        result.segments.push_back({
            static_cast<double>(trimmed_first) / kSampleRate,
            static_cast<double>(trimmed_last) / kSampleRate,
            speech_offset,
            speech_offset + duration,
        });
        speech_offset += duration;
    }
    result.speech_seconds = speech_offset;
    // A handful of retained frames is not enough to estimate perturbation
    // features; scoring it would fabricate jitter/shimmer/CPPS values. Report
    // it as no usable speech instead.
    if (!result.segments.empty()
        && result.speech_seconds < thresholds.minimum_speech_seconds) {
        result.segments.clear();
        result.speech_seconds = 0.0;
        result.rejection = VadRejection::kInsufficientSpeech;
        return result;
    }
    if (result.segments.empty()) {
        result.rejection = result.silero_segment_count == 0
            ? VadRejection::kSileroProbability
            : VadRejection::kPeriodicityBreathLike;
    }
    return result;
}

}  // namespace

VadDetector::VadDetector(
    const std::filesystem::path& model_path,
    int intra_op_threads
) {
    const char* backend = std::getenv("PITCHEE_VAD_BACKEND");
    lightweight_ = backend && std::strcmp(backend, "light") == 0;
    if (!lightweight_) {
        model_ = std::make_unique<ModelRuntime>(model_path, intra_op_threads, false);
    }
}

VadResult VadDetector::detect(
    const std::vector<float>& samples,
    const std::function<void(size_t, size_t)>& progress
) const {
    return detect(samples, general_vad_thresholds(), progress);
}

VadResult VadDetector::detect(
    const std::vector<float>& samples,
    const VadThresholds& thresholds,
    const std::function<void(size_t, size_t)>& progress
) const {
    if (samples.empty()) throw std::invalid_argument("empty audio");
    if (lightweight_) return detect_light(samples, thresholds, progress);

    std::vector<float> state(kStateSize, 0.0f);
    std::vector<float> context(kContextSize, 0.0f);
    std::vector<double> probabilities;
    probabilities.reserve((samples.size() + kWindowSize - 1) / kWindowSize);

    for (size_t start = 0; start < samples.size(); start += kWindowSize) {
        Tensor input;
        input.shape = {1, kWindowSize + kContextSize};
        input.values.reserve(kWindowSize + kContextSize);
        input.values.insert(input.values.end(), context.begin(), context.end());
        for (int index = 0; index < kWindowSize; ++index) {
            const size_t source = start + static_cast<size_t>(index);
            input.values.push_back(source < samples.size() ? samples[source] : 0.0f);
        }
        Tensor state_tensor;
        state_tensor.shape = {2, 1, 128};
        state_tensor.values = state;
        Tensor sample_rate;
        sample_rate.shape = {};
        sample_rate.data_type = Tensor::DataType::Int64;
        sample_rate.int64_values = {16000};
        std::vector<float> next_context(
            input.values.end() - kContextSize,
            input.values.end()
        );

        auto outputs = model_->run_all({
            {"input", std::move(input)},
            {"state", std::move(state_tensor)},
            {"sr", std::move(sample_rate)},
        });
        if (outputs.size() < 2 || outputs[0].values.empty()) {
            throw std::runtime_error("Silero VAD returned invalid output");
        }
        probabilities.push_back(outputs[0].values[0]);
        state = std::move(outputs[1].values);
        if (state.size() != kStateSize) {
            throw std::runtime_error("Silero VAD returned invalid state");
        }
        context = std::move(next_context);
        if (progress) {
            progress(
                std::min(samples.size(), start + kWindowSize),
                samples.size()
            );
        }
    }

    const int minimum_speech_samples = kSampleRate * 80 / 1000;
    const int minimum_silence_samples = kSampleRate * 80 / 1000;
    bool triggered = false;
    int current_start = 0;
    int temporary_end = 0;
    std::vector<RawSegment> raw_segments;

    for (size_t index = 0; index < probabilities.size(); ++index) {
        const double probability = probabilities[index];
        const int current_sample = static_cast<int>(index) * kWindowSize;
        if (probability >= thresholds.speech_start_probability && temporary_end != 0) temporary_end = 0;
        if (probability >= thresholds.speech_start_probability && !triggered) {
            triggered = true;
            current_start = current_sample;
            continue;
        }
        if (probability < thresholds.speech_end_probability && triggered) {
            if (temporary_end == 0) temporary_end = current_sample;
            const int silence_duration = current_sample - temporary_end;
            if (silence_duration < minimum_silence_samples) continue;
            if (temporary_end - current_start > minimum_speech_samples) {
                raw_segments.push_back({current_start, temporary_end});
            }
            current_start = 0;
            temporary_end = 0;
            triggered = false;
        }
    }
    if (triggered && static_cast<int>(samples.size()) - current_start > minimum_speech_samples) {
        raw_segments.push_back({current_start, static_cast<int>(samples.size())});
    }

    return finalize_vad_result(samples, std::move(raw_segments), thresholds);
}

VadResult VadDetector::detect_light(
    const std::vector<float>& samples,
    const VadThresholds& thresholds,
    const std::function<void(size_t, size_t)>& progress
) const {
    constexpr int frame_samples = 400;
    constexpr int hop_samples = 160;
    constexpr int start_frames = 2;
    constexpr int end_frames = 7;
    constexpr double absolute_floor_db = -58.0;

    std::vector<double> energy_db;
    for (size_t start = 0; start + frame_samples <= samples.size();
         start += hop_samples) {
        double energy = 0.0;
        for (int index = 0; index < frame_samples; ++index) {
            const double value = samples[start + static_cast<size_t>(index)];
            energy += value * value;
        }
        const double rms = std::sqrt(energy / frame_samples);
        energy_db.push_back(20.0 * std::log10(std::max(rms, 1e-8)));
    }
    if (energy_db.empty()) {
        return finalize_vad_result(samples, {}, thresholds);
    }

    std::vector<double> sorted = energy_db;
    std::sort(sorted.begin(), sorted.end());
    const size_t noise_index = sorted.size() / 5;
    const double noise_db = sorted[noise_index];
    const double start_threshold = std::max(absolute_floor_db, noise_db + 8.0);
    const double end_threshold = std::max(absolute_floor_db + 3.0, noise_db + 4.0);

    std::vector<RawSegment> raw_segments;
    bool triggered = false;
    int candidate_count = 0;
    int silence_count = 0;
    int current_start = 0;
    int temporary_end = 0;
    for (size_t frame = 0; frame < energy_db.size(); ++frame) {
        const int start = static_cast<int>(frame * hop_samples);
        const bool candidate = energy_db[frame] >= start_threshold;
        if (!triggered) {
            candidate_count = candidate ? candidate_count + 1 : 0;
            if (candidate_count >= start_frames) {
                triggered = true;
                current_start = std::max(
                    0,
                    start - (start_frames - 1) * hop_samples
                );
                temporary_end = start + frame_samples;
                silence_count = 0;
            }
            continue;
        }

        if (energy_db[frame] >= end_threshold) {
            temporary_end = start + frame_samples;
            silence_count = 0;
        } else {
            ++silence_count;
            if (silence_count >= end_frames) {
                raw_segments.push_back({current_start, temporary_end});
                triggered = false;
                candidate_count = 0;
                silence_count = 0;
            }
        }
        if (progress && (frame % 100 == 0 || frame + 1 == energy_db.size())) {
            progress(
                std::min(
                    samples.size(),
                    static_cast<size_t>(start + frame_samples)
                ),
                samples.size()
            );
        }
    }
    if (triggered) {
        raw_segments.push_back({
            current_start,
            static_cast<int>(samples.size())
        });
    }
    return finalize_vad_result(samples, std::move(raw_segments), thresholds);
}

}  // namespace pitchee

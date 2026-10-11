#include "voice_health.hpp"

#include "hnr.hpp"
#include "voice_health_model.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <stdexcept>

namespace pitchee {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kFrameSamples = 640;
constexpr size_t kHopSamples = 160;
constexpr size_t kFftSamples = 1024;
constexpr size_t kMaximumPatches = 32;

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if (values.size() % 2 != 0) return upper;
    std::nth_element(values.begin(), values.begin() + middle - 1, values.begin() + middle);
    return (values[middle - 1] + upper) * 0.5;
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return 0.0;
    fraction = std::max(0.0, std::min(1.0, fraction));
    const double position = fraction * static_cast<double>(values.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = static_cast<size_t>(std::ceil(position));
    std::nth_element(values.begin(), values.begin() + lower, values.end());
    const double lower_value = values[lower];
    if (lower == upper) return lower_value;
    std::nth_element(
        values.begin() + lower,
        values.begin() + upper,
        values.end()
    );
    const double upper_value = values[upper];
    const double weight = position - static_cast<double>(lower);
    return lower_value * (1.0 - weight) + upper_value * weight;
}

bool in_vad(double seconds, const VadResult& vad) {
    for (const auto& segment : vad.segments) {
        if (seconds >= segment.source_start_seconds
            && seconds < segment.source_end_seconds) {
            return true;
        }
    }
    return false;
}

double frame_rms(const std::vector<float>& signal, size_t start) {
    double sum = 0.0;
    const size_t count = std::min(kFrameSamples, signal.size() - start);
    for (size_t index = 0; index < count; ++index) {
        const double value = signal[start + index];
        sum += value * value;
    }
    return std::sqrt(sum / std::max<size_t>(count, 1));
}

void fft(std::vector<double>& real, std::vector<double>& imaginary, bool inverse) {
    const size_t size = real.size();
    for (size_t index = 1, reversed = 0; index < size; ++index) {
        size_t bit = size >> 1;
        for (; reversed & bit; bit >>= 1) reversed ^= bit;
        reversed ^= bit;
        if (index < reversed) {
            std::swap(real[index], real[reversed]);
            std::swap(imaginary[index], imaginary[reversed]);
        }
    }
    for (size_t length = 2; length <= size; length <<= 1) {
        const double sign = inverse ? 1.0 : -1.0;
        const double angle = sign * 2.0 * kPi / static_cast<double>(length);
        const double step_real = std::cos(angle);
        const double step_imaginary = std::sin(angle);
        for (size_t start = 0; start < size; start += length) {
            double phase_real = 1.0;
            double phase_imaginary = 0.0;
            const size_t half = length >> 1;
            for (size_t offset = 0; offset < half; ++offset) {
                const size_t even = start + offset;
                const size_t odd = even + half;
                const double odd_real = real[odd] * phase_real
                    - imaginary[odd] * phase_imaginary;
                const double odd_imaginary = real[odd] * phase_imaginary
                    + imaginary[odd] * phase_real;
                real[odd] = real[even] - odd_real;
                imaginary[odd] = imaginary[even] - odd_imaginary;
                real[even] += odd_real;
                imaginary[even] += odd_imaginary;
                const double next_real = phase_real * step_real
                    - phase_imaginary * step_imaginary;
                phase_imaginary = phase_real * step_imaginary
                    + phase_imaginary * step_real;
                phase_real = next_real;
            }
        }
    }
    if (inverse) {
        for (size_t index = 0; index < size; ++index) {
            real[index] /= static_cast<double>(size);
            imaginary[index] /= static_cast<double>(size);
        }
    }
}

std::optional<double> cepstral_prominence(
    const std::vector<float>& signal,
    size_t start
) {
    if (start + kFrameSamples > signal.size()) return std::nullopt;
    std::vector<double> real(kFftSamples, 0.0);
    std::vector<double> imaginary(kFftSamples, 0.0);
    double mean = 0.0;
    for (size_t index = 0; index < kFrameSamples; ++index) {
        mean += signal[start + index];
    }
    mean /= static_cast<double>(kFrameSamples);
    for (size_t index = 0; index < kFrameSamples; ++index) {
        const double hann = 0.5 - 0.5 * std::cos(
            2.0 * kPi * static_cast<double>(index)
            / static_cast<double>(kFrameSamples - 1)
        );
        real[index] = (static_cast<double>(signal[start + index]) - mean) * hann;
    }
    fft(real, imaginary, false);
    for (size_t index = 0; index < kFftSamples; ++index) {
        const double power = real[index] * real[index]
            + imaginary[index] * imaginary[index];
        real[index] = std::log(std::max(power, 1e-12));
        imaginary[index] = 0.0;
    }
    fft(real, imaginary, true);

    const size_t minimum_lag = kSampleRate / 500;
    const size_t maximum_lag = std::min<size_t>(kSampleRate / 60, kFftSamples / 2);
    size_t peak_lag = minimum_lag;
    double peak = real[peak_lag];
    double x_sum = 0.0;
    double y_sum = 0.0;
    double xx_sum = 0.0;
    double xy_sum = 0.0;
    const size_t count = maximum_lag - minimum_lag + 1;
    for (size_t lag = minimum_lag; lag <= maximum_lag; ++lag) {
        if (real[lag] > peak) {
            peak = real[lag];
            peak_lag = lag;
        }
        const double x = static_cast<double>(lag);
        x_sum += x;
        y_sum += real[lag];
        xx_sum += x * x;
        xy_sum += x * real[lag];
    }
    const double denominator = static_cast<double>(count) * xx_sum - x_sum * x_sum;
    const double slope = std::abs(denominator) > 1e-12
        ? (static_cast<double>(count) * xy_sum - x_sum * y_sum) / denominator
        : 0.0;
    const double intercept = y_sum / static_cast<double>(count)
        - slope * x_sum / static_cast<double>(count);
    return peak - (intercept + slope * static_cast<double>(peak_lag));
}

struct PerturbationFrame {
    bool valid = false;
    double period_seconds = 0.0;
    double amplitude = 0.0;
    double cpps = 0.0;
};

PerturbationFrame perturbation_frame(
    const std::vector<float>& signal,
    size_t start
) {
    PerturbationFrame result;
    if (start + kFrameSamples > signal.size()) return result;
    double mean = 0.0;
    for (size_t index = 0; index < kFrameSamples; ++index) {
        mean += signal[start + index];
    }
    mean /= static_cast<double>(kFrameSamples);
    double energy = 0.0;
    for (size_t index = 0; index < kFrameSamples; ++index) {
        const double value = static_cast<double>(signal[start + index]) - mean;
        energy += value * value;
    }
    if (energy <= 1e-12) return result;

    const size_t minimum_lag = kSampleRate / 500;
    const size_t maximum_lag = std::min<size_t>(kSampleRate / 60, kFrameSamples / 2);
    double best_correlation = -1.0;
    size_t best_lag = 0;
    for (size_t lag = minimum_lag; lag <= maximum_lag; ++lag) {
        double product = 0.0;
        double left_energy = 0.0;
        double right_energy = 0.0;
        for (size_t index = 0; index + lag < kFrameSamples; ++index) {
            const double left = static_cast<double>(signal[start + index]) - mean;
            const double right = static_cast<double>(signal[start + index + lag]) - mean;
            product += left * right;
            left_energy += left * left;
            right_energy += right * right;
        }
        const double denominator = std::sqrt(left_energy * right_energy);
        if (denominator <= 1e-12) continue;
        const double correlation = product / denominator;
        if (correlation > best_correlation) {
            best_correlation = correlation;
            best_lag = lag;
        }
    }
    if (best_correlation < 0.30 || best_lag == 0) return result;
    result.valid = true;
    result.period_seconds = static_cast<double>(best_lag) / kSampleRate;
    result.amplitude = std::sqrt(energy / static_cast<double>(kFrameSamples));
    result.cpps = cepstral_prominence(signal, start).value_or(0.0);
    return result;
}

double local_jitter(const std::vector<double>& periods) {
    if (periods.size() < 2) return 0.0;
    double sum = 0.0;
    size_t count = 0;
    for (size_t index = 0; index + 1 < periods.size(); ++index) {
        const double mean = (periods[index] + periods[index + 1]) * 0.5;
        if (mean <= 0.0) continue;
        sum += std::abs(periods[index] - periods[index + 1]) / mean;
        ++count;
    }
    return count ? sum / static_cast<double>(count) : 0.0;
}

double local_shimmer(const std::vector<double>& amplitudes) {
    if (amplitudes.size() < 2) return 0.0;
    double sum = 0.0;
    size_t count = 0;
    for (size_t index = 0; index + 1 < amplitudes.size(); ++index) {
        const double mean = (amplitudes[index] + amplitudes[index + 1]) * 0.5;
        if (mean <= 0.0) continue;
        sum += std::abs(amplitudes[index] - amplitudes[index + 1]) / mean;
        ++count;
    }
    return count ? sum / static_cast<double>(count) : 0.0;
}

double moving_perturbation(const std::vector<double>& values, size_t width) {
    if (values.size() < width) return 0.0;
    double sum = 0.0;
    size_t count = 0;
    for (size_t center = width / 2; center + width / 2 < values.size(); ++center) {
        const size_t start = center - width / 2;
        const double mean = std::accumulate(
            values.begin() + static_cast<std::ptrdiff_t>(start),
            values.begin() + static_cast<std::ptrdiff_t>(start + width),
            0.0
        ) / static_cast<double>(width);
        if (mean <= 0.0) continue;
        sum += std::abs(values[center] - mean) / mean;
        ++count;
    }
    return count ? sum / static_cast<double>(count) : 0.0;
}

double score_probability(
    const std::array<double, 14>& acoustic,
    const std::vector<std::vector<float>>& embeddings,
    std::vector<double>& ecapa_features
) {
    std::vector<double> features;
    features.reserve(kVoiceHealthFeatureCount);
    if (embeddings.empty()) return 0.0;
    std::vector<std::vector<float>> normalized = embeddings;
    for (auto& embedding : normalized) {
        normalize_l2(embedding.data(), embedding.size());
    }
    std::vector<double> raw_mean(kEmbeddingDimensions, 0.0);
    for (const auto& embedding : normalized) {
        for (size_t index = 0; index < raw_mean.size(); ++index) {
            raw_mean[index] += embedding[index];
        }
    }
    for (double& value : raw_mean) value /= static_cast<double>(normalized.size());
    std::vector<double> mean = raw_mean;
    const double mean_norm = std::sqrt(std::inner_product(
        mean.begin(), mean.end(), mean.begin(), 0.0
    ));
    if (mean_norm > 1e-12) {
        for (double& value : mean) value /= mean_norm;
    }
    for (double value : mean) features.push_back(value);
    for (size_t index = 0; index < kEmbeddingDimensions; ++index) {
        double sum = 0.0;
        for (const auto& embedding : normalized) {
            const double delta = embedding[index] - raw_mean[index];
            sum += delta * delta;
        }
        features.push_back(std::sqrt(sum / static_cast<double>(normalized.size())));
    }
    ecapa_features = features;
    for (double value : acoustic) features.push_back(value);
    if (features.size() != kVoiceHealthFeatureCount) return 0.0;

    double raw_score = kVoiceHealthIntercept;
    for (size_t index = 0; index < features.size(); ++index) {
        const double scale = std::abs(kVoiceHealthScalerScale[index]) > 1e-12
            ? kVoiceHealthScalerScale[index]
            : 1.0;
        const double standardized = (features[index] - kVoiceHealthScalerMean[index]) / scale;
        raw_score += kVoiceHealthCoefficients[index] * standardized;
    }
    return 1.0 / (1.0 + std::exp(-raw_score));
}

double map_disorder_probability(double probability) {
    constexpr double raw_threshold = kVoiceHealthRawDecisionThreshold;
    constexpr double mapped_threshold = kVoiceHealthMappedDisorderThreshold;
    probability = std::max(0.0, std::min(1.0, probability));
    if (probability <= raw_threshold) {
        return mapped_threshold * probability / raw_threshold;
    }
    return mapped_threshold
        + (1.0 - mapped_threshold)
            * (probability - raw_threshold)
            / (1.0 - raw_threshold);
}

}  // namespace

std::vector<SampleWindow> voice_health_windows(size_t sample_count) {
    if (sample_count == 0) return {};
    const size_t count = std::min(
        kMaximumPatches,
        std::max<size_t>(1, sample_count / kPatchSamples)
    );
    const size_t maximum_start = sample_count > kPatchSamples
        ? sample_count - kPatchSamples
        : 0;
    std::vector<SampleWindow> windows;
    windows.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const size_t start = count == 1
            ? 0
            : static_cast<size_t>(
                std::floor(
                    static_cast<double>(index) * maximum_start
                    / static_cast<double>(count - 1)
                )
            );
        windows.push_back({start, kPatchSamples});
    }
    return windows;
}

VoiceHealthResult analyze_voice_health(
    const std::vector<float>& signal,
    const std::vector<std::vector<float>>& embeddings,
    const PitchResult& pitch,
    const VadResult& vad
) {
    VoiceHealthResult result;

    std::vector<pitchee_hnr_vad_segment_t> segments;
    segments.reserve(vad.segments.size());
    for (const auto& segment : vad.segments) {
        segments.push_back({
            segment.source_start_seconds,
            segment.source_end_seconds,
        });
    }
    const auto hnr = analyze_hnr(
        signal.data(),
        signal.size(),
        segments.data(),
        segments.size()
    );
    result.acoustic[0] = hnr.hnr_db.value_or(0.0);

    std::vector<double> cpps_values;
    std::vector<double> periods;
    std::vector<double> amplitudes;
    std::vector<double> intensities;
    for (size_t start = 0; start + kFrameSamples <= signal.size(); start += kHopSamples) {
        const double midpoint = (
            static_cast<double>(start) + kFrameSamples * 0.5
        ) / kSampleRate;
        if (!in_vad(midpoint, vad)) continue;
        intensities.push_back(20.0 * std::log10(std::max(
            frame_rms(signal, start),
            1e-8
        )));
        const PerturbationFrame frame = perturbation_frame(signal, start);
        if (!frame.valid) continue;
        periods.push_back(frame.period_seconds);
        amplitudes.push_back(frame.amplitude);
        cpps_values.push_back(100.0 + frame.cpps);
    }

    result.acoustic[1] = median(cpps_values);
    result.acoustic[2] = local_jitter(periods);
    result.acoustic[3] = moving_perturbation(periods, 3);
    result.acoustic[4] = moving_perturbation(periods, 5);
    result.acoustic[5] = local_shimmer(amplitudes);
    result.acoustic[6] = moving_perturbation(amplitudes, 3);
    result.acoustic[7] = moving_perturbation(amplitudes, 5);

    std::vector<double> f0;
    for (size_t index = 0; index < pitch.pitch_hz.size(); ++index) {
        if (pitch.voicing[index]) f0.push_back(pitch.pitch_hz[index]);
    }
    result.acoustic[8] = median(f0);
    result.acoustic[9] = percentile(f0, 0.75) - percentile(f0, 0.25);
    const double f0_mean = f0.empty()
        ? 0.0
        : std::accumulate(f0.begin(), f0.end(), 0.0) / static_cast<double>(f0.size());
    double f0_variance = 0.0;
    for (double value : f0) {
        f0_variance += (value - f0_mean) * (value - f0_mean);
    }
    result.acoustic[10] = f0_mean > 0.0
        ? std::sqrt(f0_variance / static_cast<double>(f0.size())) / f0_mean
        : 0.0;
    result.acoustic[11] = pitch.pitch_hz.empty()
        ? 0.0
        : static_cast<double>(f0.size()) / static_cast<double>(pitch.pitch_hz.size());
    result.acoustic[12] = median(intensities);
    result.acoustic[13] = percentile(intensities, 0.75) - percentile(intensities, 0.25);

    const double raw_disorder_probability = score_probability(
        result.acoustic,
        embeddings,
        result.ecapa_features
    );
    result.disorder_probability = map_disorder_probability(raw_disorder_probability);
    result.health_score = 100.0 * (1.0 - result.disorder_probability);
    result.disordered = result.health_score < kVoiceHealthHealthScoreThreshold;
    return result;
}

}  // namespace pitchee

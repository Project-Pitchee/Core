#include "v0_audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace pitchee::v0 {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kKaiserBeta = 5.0;

double bessel_i0(double value) {
    const double half = value / 2.0;
    double sum = 1.0;
    double term = 1.0;
    for (int order = 1; order < 128; ++order) {
        term *= (half / order) * (half / order);
        sum += term;
        if (term <= sum * 1e-17) break;
    }
    return sum;
}

double sinc(double value) {
    if (std::abs(value) < 1e-15) return 1.0;
    return std::sin(kPi * value) / (kPi * value);
}

std::vector<double> scipy_lowpass(int64_t tap_count, double cutoff) {
    std::vector<double> filter(static_cast<size_t>(tap_count));
    const double center = static_cast<double>(tap_count - 1) / 2.0;
    const double denominator = bessel_i0(kKaiserBeta);
    double sum = 0.0;
    for (int64_t index = 0; index < tap_count; ++index) {
        const double position = static_cast<double>(index);
        const double window_position = 2.0 * position / static_cast<double>(tap_count - 1) - 1.0;
        const double window = bessel_i0(
            kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - window_position * window_position))
        ) / denominator;
        const double value = cutoff * sinc(cutoff * (position - center)) * window;
        filter[static_cast<size_t>(index)] = value;
        sum += value;
    }
    for (double& value : filter) value /= sum;
    return filter;
}

double quantile_linear(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = q * static_cast<double>(values.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = std::min(lower + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

std::vector<double> frame_rms_db(const std::vector<float>& signal) {
    if (signal.size() < kFrameSamples) return {};
    const size_t frame_count = (signal.size() - kFrameSamples) / kHopSamples + 1;
    std::vector<double> values(frame_count);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        double energy = 0.0;
        const size_t start = frame * kHopSamples;
        for (int index = 0; index < kFrameSamples; ++index) {
            const double value = signal[start + static_cast<size_t>(index)];
            energy += value * value;
        }
        const double rms = std::sqrt(energy / static_cast<double>(kFrameSamples) + 1e-12);
        values[frame] = 20.0 * std::log10(std::max(rms, 1e-8));
    }
    return values;
}

struct CropResult {
    std::vector<float> samples;
    double speech_seconds = 0.0;
    int active_frames = 0;
    int64_t crop_start_sample = 0;
};

CropResult trim_and_crop(const std::vector<float>& signal) {
    CropResult result;
    if (signal.size() < kFrameSamples) {
        result.samples = signal;
        return result;
    }

    const std::vector<double> rms_db = frame_rms_db(signal);
    if (rms_db.empty()) {
        result.samples = signal;
        return result;
    }
    const double low = quantile_linear(rms_db, 0.10);
    const double high = quantile_linear(rms_db, 0.95);
    const double threshold = std::max(low + 8.0, high - 35.0);
    std::vector<uint8_t> active(rms_db.size(), 0);
    int active_count = 0;
    for (size_t index = 0; index < rms_db.size(); ++index) {
        active[index] = rms_db[index] >= threshold ? 1 : 0;
        active_count += active[index];
    }
    if (active_count < std::max(3, static_cast<int>(0.01 * static_cast<double>(active.size())))) {
        active_count = 0;
        for (size_t index = 0; index < rms_db.size(); ++index) {
            active[index] = rms_db[index] >= high - 45.0 ? 1 : 0;
            active_count += active[index];
        }
    }
    result.active_frames = active_count;
    result.speech_seconds = static_cast<double>(active_count * kHopSamples)
        / static_cast<double>(kSampleRate);
    if (active_count == 0) {
        result.samples = signal;
        return result;
    }

    size_t first = 0;
    while (first < active.size() && active[first] == 0) ++first;
    size_t last = active.size() - 1;
    while (last > 0 && active[last] == 0) --last;

    constexpr int64_t margin = kSampleRate * 15 / 100;
    int64_t start = std::max<int64_t>(
        0,
        static_cast<int64_t>(first) * kHopSamples - margin
    );
    int64_t end = std::min<int64_t>(
        static_cast<int64_t>(signal.size()),
        static_cast<int64_t>(last) * kHopSamples + kFrameSamples + margin
    );
    std::vector<float> cropped(
        signal.begin() + static_cast<std::ptrdiff_t>(start),
        signal.begin() + static_cast<std::ptrdiff_t>(end)
    );

    if (cropped.size() > static_cast<size_t>(kMaximumSamples)) {
        const int64_t frame_start = std::max<int64_t>(0, start / kHopSamples);
        const int64_t frame_end = std::min<int64_t>(
            static_cast<int64_t>(active.size()),
            frame_start + static_cast<int64_t>(cropped.size()) / kHopSamples
        );
        constexpr int64_t window_frames = kMaximumSamples / kHopSamples;
        const int64_t local_count = frame_end - frame_start;
        if (local_count > window_frames) {
            std::vector<int64_t> cumulative(static_cast<size_t>(local_count + 1), 0);
            for (int64_t index = 0; index < local_count; ++index) {
                cumulative[static_cast<size_t>(index + 1)] = cumulative[static_cast<size_t>(index)]
                    + active[static_cast<size_t>(frame_start + index)];
            }
            int64_t best = 0;
            int64_t best_score = -1;
            const int64_t score_count = local_count - window_frames + 1;
            for (int64_t index = 0; index < score_count; ++index) {
                const int64_t score = cumulative[static_cast<size_t>(index + window_frames)]
                    - cumulative[static_cast<size_t>(index)];
                if (score > best_score) {
                    best_score = score;
                    best = index;
                }
            }
            // vf3 reports this offset relative to the trimmed crop. The trim
            // start is hop-aligned, so adding it preserves the same window.
            const int64_t relative_start = std::max<int64_t>(
                0,
                best * kHopSamples - margin
            );
            const int64_t relative_end = std::min<int64_t>(
                static_cast<int64_t>(cropped.size()),
                relative_start + kMaximumSamples
            );
            cropped.assign(
                cropped.begin() + static_cast<std::ptrdiff_t>(relative_start),
                cropped.begin() + static_cast<std::ptrdiff_t>(relative_end)
            );
            start += relative_start;
            end = start + static_cast<int64_t>(cropped.size());
        } else {
            cropped.resize(static_cast<size_t>(kMaximumSamples));
            end = start + kMaximumSamples;
        }
    }
    result.samples = std::move(cropped);
    result.crop_start_sample = start;
    return result;
}

}  // namespace

std::vector<float> average_channels(
    const float* interleaved,
    size_t sample_count,
    int channels
) {
    if (!interleaved || channels <= 0 || sample_count % static_cast<size_t>(channels) != 0) {
        throw std::invalid_argument("invalid interleaved PCM buffer");
    }
    const size_t frame_count = sample_count / static_cast<size_t>(channels);
    std::vector<float> mono(frame_count);
    for (size_t frame = 0; frame < frame_count; ++frame) {
        double sum = 0.0;
        for (int channel = 0; channel < channels; ++channel) {
            sum += interleaved[frame * static_cast<size_t>(channels)
                + static_cast<size_t>(channel)];
        }
        mono[frame] = static_cast<float>(sum / static_cast<double>(channels));
    }
    return mono;
}

std::vector<float> resample_poly(
    const float* input,
    size_t input_samples,
    int input_rate,
    int output_rate
) {
    if (!input || input_samples == 0 || input_rate <= 0 || output_rate <= 0) return {};
    if (input_rate == output_rate) {
        return std::vector<float>(input, input + input_samples);
    }

    const int divisor = std::gcd(input_rate, output_rate);
    const int up = output_rate / divisor;
    const int down = input_rate / divisor;
    const int64_t maximum_rate = std::max(up, down);
    const int64_t half_length = 10 * maximum_rate;
    std::vector<double> filter = scipy_lowpass(2 * half_length + 1, 1.0 / maximum_rate);
    for (double& value : filter) value *= static_cast<double>(up);

    const int64_t pre_pad = down - (half_length % down);
    int64_t post_pad = 0;
    const auto output_length = [](int64_t filter_length, size_t samples, int up_rate, int down_rate) {
        const int64_t numerator = static_cast<int64_t>(samples - 1) * up_rate + filter_length;
        return (numerator + down_rate - 1) / down_rate;
    };
    const int64_t output_count = (static_cast<int64_t>(input_samples) * up + down - 1) / down;
    const int64_t pre_remove = (half_length + pre_pad) / down;
    while (output_length(static_cast<int64_t>(filter.size()) + pre_pad + post_pad,
                         input_samples, up, down) < output_count + pre_remove) {
        ++post_pad;
    }

    std::vector<double> padded(
        static_cast<size_t>(pre_pad + static_cast<int64_t>(filter.size()) + post_pad),
        0.0
    );
    std::copy(filter.begin(), filter.end(), padded.begin() + static_cast<std::ptrdiff_t>(pre_pad));

    std::vector<float> output(static_cast<size_t>(output_count));
    const int64_t filter_length = static_cast<int64_t>(padded.size());
    for (int64_t output_index = 0; output_index < output_count; ++output_index) {
        const int64_t center = (output_index + pre_remove) * down;
        const int64_t first_input = std::max<int64_t>(
            0,
            (center - filter_length + 1 + up - 1) / up
        );
        const int64_t last_input = std::min<int64_t>(
            static_cast<int64_t>(input_samples) - 1,
            center / up
        );
        double value = 0.0;
        for (int64_t source = first_input; source <= last_input; ++source) {
            const int64_t filter_index = center - source * up;
            value += padded[static_cast<size_t>(filter_index)]
                * static_cast<double>(input[static_cast<size_t>(source)]);
        }
        output[static_cast<size_t>(output_index)] = static_cast<float>(value);
    }
    return output;
}

ProcessedAudio prepare_audio(
    const float* interleaved,
    size_t sample_count,
    int sample_rate,
    int channels
) {
    std::vector<float> mono = average_channels(interleaved, sample_count, channels);
    if (sample_rate != kSampleRate) {
        mono = resample_poly(mono.data(), mono.size(), sample_rate, kSampleRate);
    }
    if (mono.empty()) throw std::invalid_argument("audio is empty");
    double mean = 0.0;
    for (float value : mono) mean += value;
    const float subtraction = static_cast<float>(mean / static_cast<double>(mono.size()));
    for (float& value : mono) value -= subtraction;

    ProcessedAudio result;
    result.metadata.source_seconds = static_cast<double>(mono.size()) / kSampleRate;
    CropResult crop = trim_and_crop(mono);
    result.metadata.speech_seconds = crop.speech_seconds;
    result.metadata.active_frames = crop.active_frames;
    result.metadata.crop_start_sample = crop.crop_start_sample;
    result.samples = std::move(crop.samples);
    if (result.samples.size() > static_cast<size_t>(kMaximumSamples)) {
        result.samples.resize(static_cast<size_t>(kMaximumSamples));
    }
    if (result.samples.size() < static_cast<size_t>(kMinimumSamples)) {
        result.samples.resize(static_cast<size_t>(kMinimumSamples), 0.0f);
    }
    result.metadata.analyzed_seconds = static_cast<double>(result.samples.size()) / kSampleRate;
    return result;
}

std::vector<float> normalize_waveform(const std::vector<float>& samples) {
    if (samples.empty()) return {};
    double mean = 0.0;
    for (float value : samples) mean += static_cast<double>(value);
    mean /= static_cast<double>(samples.size());
    double variance = 0.0;
    for (float value : samples) {
        const double delta = static_cast<double>(value) - mean;
        variance += delta * delta;
    }
    variance /= static_cast<double>(samples.size());
    const double divisor = std::sqrt(variance + 1e-7);
    std::vector<float> output(samples.size());
    for (size_t index = 0; index < samples.size(); ++index) {
        output[index] = static_cast<float>(
            (static_cast<double>(samples[index]) - mean) / divisor
        );
    }
    return output;
}

}  // namespace pitchee::v0

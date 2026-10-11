#include "hnr.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pitchee {
namespace {

struct HNRStatistics {
    int count = 0;
    double mean = 0.0;
    double squared_deviations = 0.0;

    void add(const std::optional<double>& value) noexcept {
        if (!value) return;
        ++count;
        const double delta = *value - mean;
        mean += delta / static_cast<double>(count);
        squared_deviations += delta * (*value - mean);
    }

    double standard_deviation() const noexcept {
        return count > 0
            ? std::sqrt(std::max(0.0, squared_deviations / static_cast<double>(count)))
            : 0.0;
    }

    void apply(VoiceQualityResult& result) const noexcept {
        result.hnr_window_count = count;
        result.hnr_db = count > 0 ? std::optional<double>(mean) : std::nullopt;
        result.hnr_std_db = count > 0
            ? std::optional<double>(standard_deviation())
            : std::nullopt;
    }
};

bool valid_vad_segments(
    const pitchee_hnr_vad_segment_t* segments,
    size_t count
) noexcept {
    if (!segments && count > 0) return false;
    double previous_end = 0.0;
    for (size_t index = 0; index < count; ++index) {
        const auto& segment = segments[index];
        if (!std::isfinite(segment.start_seconds) || !std::isfinite(segment.end_seconds)
            || segment.start_seconds < 0.0 || segment.start_seconds >= segment.end_seconds
            || (index > 0 && segment.start_seconds < previous_end)) {
            return false;
        }
        previous_end = segment.end_seconds;
    }
    return true;
}

template <typename Callback>
HNRStatistics visit_hnr_windows(
    const float* samples,
    size_t sample_count,
    const pitchee_hnr_vad_segment_t* vad_segments,
    size_t vad_segment_count,
    const Callback& callback
) {
    HNRStatistics statistics;
    size_t segment_index = 0;
    for (size_t start = 0; start < sample_count;) {
        const size_t remaining = sample_count - start;
        const size_t length = std::min(kHNRWindowSamples, remaining);
        HNRWindow window{
            static_cast<double>(start) / kHNRSampleRate,
            static_cast<double>(start + length) / kHNRSampleRate,
            std::nullopt,
        };
        const double midpoint = (static_cast<double>(start) + length * 0.5) / kHNRSampleRate;
        while (segment_index < vad_segment_count
               && vad_segments[segment_index].end_seconds <= midpoint) {
            ++segment_index;
        }
        // VAD is the first gate: rejected windows neither inspect PCM nor run
        // autocorrelation. Retain their geometry and nil values in the output.
        if (segment_index < vad_segment_count
            && vad_segments[segment_index].start_seconds <= midpoint) {
            window = calculate_hnr_window(samples + start, length, start);
        }
        statistics.add(window.hnr_db);
        callback(window);
        if (remaining <= kHNRHopSamples) break;
        start += kHNRHopSamples;
    }
    return statistics;
}

}  // namespace

std::optional<double> hnr_from_autocorrelation(double correlation) noexcept {
    if (!std::isfinite(correlation) || correlation < 0.3) return std::nullopt;
    // A perfectly periodic signal can round to 1. Preserve a finite JSON value
    // using the nearest representable value below 1, without imposing a dB cap.
    const double peak = std::min(correlation, std::nextafter(1.0, 0.0));
    return 10.0 * std::log10(peak / (1.0 - peak));
}

HNRWindow calculate_hnr_window(
    const float* samples,
    size_t sample_count,
    size_t start_sample
) noexcept {
    HNRWindow result{
        static_cast<double>(start_sample) / kHNRSampleRate,
        static_cast<double>(start_sample + sample_count) / kHNRSampleRate,
        std::nullopt,
    };
    if (!samples) return result;
    constexpr size_t minimum_lag = (kHNRSampleRate + 600 - 1) / 600;
    constexpr size_t maximum_lag = kHNRSampleRate / 75;
    // A lag needs at least two periods in the window. Otherwise a short tail
    // could leave just one overlapping pair and falsely report r = 1.
    const size_t last_lag = std::min(maximum_lag, sample_count / 2);
    if (last_lag < minimum_lag) return result;

    double mean = 0.0;
    for (size_t index = 0; index < sample_count; ++index) {
        if (!std::isfinite(samples[index])) return result;
        mean += samples[index];
    }
    mean /= static_cast<double>(sample_count);

    double peak = 0.0;
    for (size_t lag = minimum_lag; lag <= last_lag; ++lag) {
        double product = 0.0;
        double left_energy = 0.0;
        double right_energy = 0.0;
        for (size_t index = 0; index < sample_count - lag; ++index) {
            const double left = static_cast<double>(samples[index]) - mean;
            const double right = static_cast<double>(samples[index + lag]) - mean;
            product += left * right;
            left_energy += left * left;
            right_energy += right * right;
        }
        // Normalize over the actual overlap, avoiding finite-window attenuation
        // of periodic signals. Centering removes a constant microphone offset.
        const double denominator = std::sqrt(left_energy * right_energy);
        if (denominator > 0.0) peak = std::max(peak, product / denominator);
    }
    result.hnr_db = hnr_from_autocorrelation(peak);
    return result;
}

void summarize_hnr(VoiceQualityResult& result) noexcept {
    HNRStatistics statistics;
    for (const auto& window : result.windows) statistics.add(window.hnr_db);
    statistics.apply(result);
}

VoiceQualityResult analyze_hnr(
    const float* samples,
    size_t sample_count,
    const pitchee_hnr_vad_segment_t* vad_segments,
    size_t vad_segment_count
) {
    if ((!samples && sample_count > 0) || !valid_vad_segments(vad_segments, vad_segment_count)) {
        throw std::invalid_argument("invalid HNR PCM or VAD argument");
    }
    VoiceQualityResult result;
    result.windows.reserve(sample_count / kHNRHopSamples + (sample_count % kHNRHopSamples != 0));
    const auto statistics = visit_hnr_windows(samples, sample_count, vad_segments, vad_segment_count, [&result](const HNRWindow& window) {
        result.windows.push_back(window);
    });
    statistics.apply(result);
    return result;
}

}  // namespace pitchee

extern "C" pitchee_status_t pitchee_hnr_analyze(
    const float* samples,
    size_t sample_count,
    const pitchee_hnr_vad_segment_t* vad_segments,
    size_t vad_segment_count,
    pitchee_hnr_window_callback_t window_callback,
    void* user_data,
    pitchee_hnr_summary_t* out_summary
) {
    if (out_summary) {
        *out_summary = {};
        out_summary->window_seconds = pitchee::kHNRWindowSeconds;
    }
    if (!out_summary || (!samples && sample_count > 0)
        || !pitchee::valid_vad_segments(vad_segments, vad_segment_count)) {
        return PITCHEE_ERROR_INVALID_ARGUMENT;
    }
    try {
        const auto statistics = pitchee::visit_hnr_windows(samples, sample_count, vad_segments, vad_segment_count, [&](const pitchee::HNRWindow& window) {
            if (!window_callback) return;
            const pitchee_hnr_window_t frame{
                window.start_seconds,
                window.end_seconds,
                window.hnr_db.value_or(0.0),
                window.hnr_db ? 1 : 0,
                0,
            };
            window_callback(&frame, user_data);
        });
        out_summary->hnr_db = statistics.mean;
        out_summary->hnr_std_db = statistics.standard_deviation();
        out_summary->hnr_window_count = statistics.count;
        out_summary->has_hnr = statistics.count > 0 ? 1 : 0;
        return PITCHEE_SUCCESS;
    } catch (...) {
        return PITCHEE_ERROR_INTERNAL;
    }
}

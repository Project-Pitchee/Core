#include "hnr.hpp"
#include "pitchee/hnr.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

namespace {

size_t allocation_count = 0;

struct WindowCapture {
    std::array<pitchee_hnr_window_t, 8> windows{};
    size_t count = 0;
    bool call_active = false;
    bool all_callbacks_synchronous = true;
};

void capture_window(const pitchee_hnr_window_t* window, void* user_data) {
    auto& capture = *static_cast<WindowCapture*>(user_data);
    capture.all_callbacks_synchronous &= capture.call_active;
    if (capture.count < capture.windows.size()) capture.windows[capture.count] = *window;
    ++capture.count;
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

bool near(double lhs, double rhs, double tolerance = 1e-10) {
    return std::abs(lhs - rhs) < tolerance;
}

std::vector<float> noise(size_t sample_count) {
    std::vector<float> result(sample_count);
    uint32_t state = 0x12345678;
    for (auto& sample : result) {
        state = 1664525 * state + 1013904223;
        sample = static_cast<float>(static_cast<double>(state) / 4294967296.0 - 0.5);
    }
    return result;
}

}  // namespace

void* operator new(std::size_t size) {
    ++allocation_count;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }

int main() {
    const auto threshold = pitchee::hnr_from_autocorrelation(0.3);
    require(threshold && near(*threshold, 10.0 * std::log10(0.3 / 0.7)),
            "r = 0.3 is included and follows the Praat formula");
    require(!pitchee::hnr_from_autocorrelation(std::nextafter(0.3, 0.0)),
            "r immediately below 0.3 is unavailable");
    require(!pitchee::hnr_from_autocorrelation(0.0), "zero correlation is unavailable");
    require(!pitchee::hnr_from_autocorrelation(-1.0), "negative correlation is unavailable");
    require(!pitchee::hnr_from_autocorrelation(std::numeric_limits<double>::quiet_NaN()),
            "NaN correlation is unavailable");
    require(!pitchee::hnr_from_autocorrelation(std::numeric_limits<double>::infinity()),
            "infinite correlation is unavailable");
    require(near(*pitchee::hnr_from_autocorrelation(0.5), 0.0), "r = 0.5 is 0 dB");
    const double nearly_one = std::nextafter(1.0, 0.0);
    const auto nearly_perfect = pitchee::hnr_from_autocorrelation(nearly_one);
    require(nearly_perfect && std::isfinite(*nearly_perfect)
            && near(*nearly_perfect, 10.0 * std::log10(nearly_one / (1.0 - nearly_one))),
            "r approaching 1 remains finite and follows the formula");
    require(pitchee::hnr_from_autocorrelation(1.0) == nearly_perfect,
            "perfect periodicity uses the nearest representable r below 1");
    require(pitchee::hnr_from_autocorrelation(std::nextafter(1.0, 2.0)) == nearly_perfect,
            "correlation rounding above 1 cannot create infinity or NaN");

    constexpr size_t window_samples = pitchee::kHNRWindowSamples;
    const pitchee_hnr_vad_segment_t full_vad{0.0, 1.0};
    constexpr double pi = 3.14159265358979323846;
    std::vector<float> tone(window_samples);
    for (size_t index = 0; index < tone.size(); ++index) {
        tone[index] = static_cast<float>(0.4 * std::sin(2.0 * pi * 200.0 * index / pitchee::kHNRSampleRate));
    }
    const size_t allocations_before_hnr = allocation_count;
    const auto periodic = pitchee::calculate_hnr_window(tone.data(), tone.size(), 1600);
    require(allocation_count == allocations_before_hnr, "per-window HNR performs zero heap allocations");
    require(periodic.hnr_db && std::isfinite(*periodic.hnr_db) && *periodic.hnr_db > 80.0,
            "periodic PCM has finite high HNR without any F0 model or confidence gate");
    require(periodic.start_seconds == 0.1 && periodic.end_seconds == 0.14,
            "HNR bounds come from independent PCM sample positions");
    const auto random = noise(window_samples);
    require(!pitchee::calculate_hnr_window(random.data(), random.size(), 1600).hnr_db,
            "aperiodic PCM is rejected by its own correlation threshold");
    const std::vector<float> silence(window_samples, 0.0f);
    require(!pitchee::calculate_hnr_window(silence.data(), silence.size(), 1600).hnr_db,
            "silent PCM propagates nil instead of dividing by zero");
    const std::vector<float> constant(window_samples, 0.25f);
    require(!pitchee::calculate_hnr_window(constant.data(), constant.size(), 1600).hnr_db,
            "DC alone is not periodic voice");
    require(!pitchee::calculate_hnr_window(tone.data(), 53, 1600).hnr_db,
            "a tail shorter than two periods at the minimum lag propagates nil");
    require(!pitchee::calculate_hnr_window(random.data(), 53, 1600).hnr_db,
            "short noise cannot obtain r = 1 from a single overlapping pair");
    require(!pitchee::calculate_hnr_window(nullptr, 0, 1600).hnr_db,
            "empty PCM propagates nil");
    tone[42] = std::numeric_limits<float>::quiet_NaN();
    require(!pitchee::calculate_hnr_window(tone.data(), tone.size(), 1600).hnr_db,
            "invalid PCM propagates nil");

    for (const double frequency : {75.0, 600.0}) {
        for (size_t index = 0; index < tone.size(); ++index) {
            tone[index] = static_cast<float>(0.4 * std::sin(2.0 * pi * frequency * index / pitchee::kHNRSampleRate));
        }
        const auto edge = pitchee::calculate_hnr_window(tone.data(), tone.size(), 1600);
        require(edge.hnr_db && *edge.hnr_db > 30.0, "lag search covers the F0 band edges");
    }

    std::vector<float> offset_scaled(window_samples);
    for (size_t index = 0; index < tone.size(); ++index) {
        tone[index] = static_cast<float>(0.3 * std::sin(2.0 * pi * 200.0 * index / pitchee::kHNRSampleRate)
                                        + 0.5 * random[index]);
        offset_scaled[index] = 0.3f * tone[index] + 0.2f;
    }
    const auto mixed = pitchee::calculate_hnr_window(tone.data(), tone.size(), 1600);
    const auto transformed = pitchee::calculate_hnr_window(offset_scaled.data(), offset_scaled.size(), 1600);
    require(mixed.hnr_db && transformed.hnr_db && *mixed.hnr_db < *periodic.hnr_db,
            "adding noise lowers HNR");
    require(near(*mixed.hnr_db, *transformed.hnr_db, 1e-5),
            "normalized centered correlation is independent of gain and DC offset");

    // This executable links only hnr.cpp: this public C API cannot depend on
    // analyzer construction, an F0 gate, ONNX Runtime, or any model files.
    pitchee_hnr_summary_t external{};
    WindowCapture capture;
    capture.call_active = true;
    const size_t allocations_before_api = allocation_count;
    const auto status = pitchee_hnr_analyze(tone.data(), tone.size(), &full_vad, 1, capture_window, &capture, &external);
    capture.call_active = false;
    require(status == PITCHEE_SUCCESS && allocation_count == allocations_before_api,
            "HNR consumes PCM and supplied VAD with zero heap allocations and no model");
    require(capture.all_callbacks_synchronous && capture.count == 4,
            "HNR emits independent 10 ms starts synchronously before return");
    require(external.window_seconds == 0.04 && external.has_hnr && external.hnr_window_count > 0,
            "HNR reports its own 40 ms window and valid correlation summary");
    const auto independent = pitchee::analyze_hnr(tone.data(), tone.size(), &full_vad, 1);
    require(independent.window_seconds == 0.04 && independent.windows.size() == capture.count,
            "offline wrapper preserves the independent external timeline");
    require(independent.hnr_db && near(*independent.hnr_db, external.hnr_db)
            && independent.hnr_std_db && near(*independent.hnr_std_db, external.hnr_std_db)
            && independent.hnr_window_count == external.hnr_window_count,
            "offline and allocation-free external API share identical aggregate results");
    for (size_t index = 0; index < capture.count; ++index) {
        const auto& frame = capture.windows[index];
        const auto& window = independent.windows[index];
        require(frame.start_seconds == index * 0.01 && frame.end_seconds == 0.04,
                "HNR uses 10 ms hops and retains overlapping partial trailing windows");
        require(window.start_seconds == frame.start_seconds && window.end_seconds == frame.end_seconds
                && window.hnr_db.has_value() == (frame.has_hnr != 0)
                && (!window.hnr_db || *window.hnr_db == frame.hnr_db),
                "C window copies and C++ windows preserve values and nil identically");
    }
    const auto saved_frame = capture.windows[0];
    require(pitchee_hnr_analyze(silence.data(), silence.size(), &full_vad, 1, nullptr, nullptr, &external) == PITCHEE_SUCCESS
            && !external.has_hnr && external.hnr_window_count == 0,
            "silence propagates an unavailable external summary without a callback");
    require(capture.windows[0].hnr_db == saved_frame.hnr_db,
            "copied window data remains valid after a later HNR call");
    require(pitchee_hnr_analyze(nullptr, 0, nullptr, 0, nullptr, nullptr, &external) == PITCHEE_SUCCESS
            && !external.has_hnr && external.hnr_window_count == 0,
            "empty external input succeeds with an unavailable summary");
    require(pitchee_hnr_analyze(nullptr, 1, nullptr, 0, nullptr, nullptr, &external) == PITCHEE_ERROR_INVALID_ARGUMENT,
            "nonempty external input requires a PCM pointer");
    require(pitchee_hnr_analyze(tone.data(), tone.size(), &full_vad, 1, nullptr, nullptr, nullptr) == PITCHEE_ERROR_INVALID_ARGUMENT,
            "external output summary is required");
    const auto empty = pitchee::analyze_hnr(nullptr, 0, nullptr, 0);
    require(empty.windows.empty() && !empty.hnr_db && empty.hnr_window_count == 0,
            "offline empty PCM propagates an empty unavailable result");
    auto partial = noise(801);
    const auto partial_result = pitchee::analyze_hnr(partial.data(), partial.size(), &full_vad, 1);
    require(partial_result.windows.size() == 6
            && partial_result.windows.back().start_seconds == 0.05
            && partial_result.windows.back().end_seconds == 801.0 / pitchee::kHNRSampleRate
            && !partial_result.windows.back().hnr_db,
            "a one-sample final tail is included on the independent grid and remains nil");

    std::vector<float> gate_pcm(1600);
    for (size_t index = 0; index < gate_pcm.size(); ++index) {
        gate_pcm[index] = static_cast<float>(0.4 * std::sin(2.0 * pi * 200.0 * index / pitchee::kHNRSampleRate));
    }
    const std::array<pitchee_hnr_vad_segment_t, 2> source_vad{{{0.02, 0.03}, {0.06, 0.075}}};
    const auto gated = pitchee::analyze_hnr(gate_pcm.data(), gate_pcm.size(), source_vad.data(), source_vad.size());
    require(gated.windows.size() == 10 && gated.hnr_window_count == 3,
            "VAD gating preserves the independent grid but counts only in-segment HNR");
    for (size_t index = 0; index < gated.windows.size(); ++index) {
        const bool expected = index == 0 || index == 4 || index == 5;
        require(gated.windows[index].hnr_db.has_value() == expected,
                "only centers inside source VAD intervals pass, with inclusive start and exclusive end");
    }
    const auto no_vad = pitchee::analyze_hnr(gate_pcm.data(), gate_pcm.size(), nullptr, 0);
    require(no_vad.windows.size() == 10 && !no_vad.hnr_db && !no_vad.hnr_std_db
            && no_vad.hnr_window_count == 0,
            "empty VAD rejects even perfect periodic PCM instead of bypassing the gate");
    for (const auto& window : no_vad.windows) require(!window.hnr_db, "non-VAD windows remain nil");
    const pitchee_hnr_vad_segment_t tail_vad{0.065, 0.071};
    const auto tails = pitchee::analyze_hnr(gate_pcm.data(), 1120, &tail_vad, 1);
    require(tails.windows.size() == 7 && tails.windows[6].hnr_db
            && !tails.windows[5].hnr_db && tails.hnr_window_count == 1,
            "a partial tail uses its actual clipped center rather than the nominal 40 ms center");
    const pitchee_hnr_vad_segment_t beyond_input{1.0, 2.0};
    require(pitchee_hnr_analyze(gate_pcm.data(), gate_pcm.size(), &beyond_input, 1,
                               nullptr, nullptr, &external) == PITCHEE_SUCCESS
            && !external.has_hnr,
            "valid VAD beyond input bounds is ignored without reading outside PCM");
    const std::array<pitchee_hnr_vad_segment_t, 2> adjacent{{{0.0, 0.04}, {0.04, 1.0}}};
    require(pitchee_hnr_analyze(gate_pcm.data(), gate_pcm.size(), adjacent.data(), adjacent.size(),
                               nullptr, nullptr, &external) == PITCHEE_SUCCESS
            && external.hnr_window_count == 10,
            "adjacent intervals are allowed and do not omit their shared boundary");
    for (const pitchee_hnr_vad_segment_t invalid : {
        pitchee_hnr_vad_segment_t{-0.01, 0.1},
        pitchee_hnr_vad_segment_t{0.1, 0.1},
        pitchee_hnr_vad_segment_t{0.2, 0.1},
        pitchee_hnr_vad_segment_t{0.0, std::numeric_limits<double>::infinity()},
        pitchee_hnr_vad_segment_t{std::numeric_limits<double>::quiet_NaN(), 0.1}
    }) {
        require(pitchee_hnr_analyze(gate_pcm.data(), gate_pcm.size(), &invalid, 1,
                                   nullptr, nullptr, &external) == PITCHEE_ERROR_INVALID_ARGUMENT,
                "invalid VAD interval values are rejected before analysis");
    }
    const std::array<pitchee_hnr_vad_segment_t, 2> overlapping{{{0.0, 0.05}, {0.04, 0.1}}};
    const std::array<pitchee_hnr_vad_segment_t, 2> unsorted{{{0.05, 0.1}, {0.0, 0.04}}};
    require(pitchee_hnr_analyze(gate_pcm.data(), gate_pcm.size(), overlapping.data(), 2,
                               nullptr, nullptr, &external) == PITCHEE_ERROR_INVALID_ARGUMENT,
            "overlapping VAD intervals are rejected");
    require(pitchee_hnr_analyze(gate_pcm.data(), gate_pcm.size(), unsorted.data(), 2,
                               nullptr, nullptr, &external) == PITCHEE_ERROR_INVALID_ARGUMENT,
            "unsorted VAD intervals are rejected");
    require(pitchee_hnr_analyze(gate_pcm.data(), gate_pcm.size(), nullptr, 1,
                               nullptr, nullptr, &external) == PITCHEE_ERROR_INVALID_ARGUMENT,
            "nonempty VAD list requires a segment pointer");

    pitchee::VoiceQualityResult summary;
    summary.windows = {
        {0.0, 0.05, std::nullopt},
        {0.05, 0.1, -3.0},
        {0.1, 0.125, 5.0},
    };
    const size_t allocations_before_summary = allocation_count;
    pitchee::summarize_hnr(summary);
    require(allocation_count == allocations_before_summary, "HNR aggregation allocates nothing");
    require(summary.hnr_window_count == 2 && summary.hnr_db && near(*summary.hnr_db, 1.0),
            "mean counts only available HNR windows");
    require(summary.hnr_std_db && near(*summary.hnr_std_db, 4.0),
            "standard deviation is population standard deviation");
    require(summary.windows.size() == 3 && !summary.windows[0].hnr_db
            && summary.windows[2].end_seconds == 0.125,
            "aggregation preserves nil windows and a partial final window");
    summary.windows = {{0.0, 0.05, 2.0}};
    pitchee::summarize_hnr(summary);
    require(summary.hnr_window_count == 1 && summary.hnr_std_db == 0.0,
            "single valid window has zero population standard deviation");
    summary.windows = {{0.0, 0.05, std::nullopt}};
    pitchee::summarize_hnr(summary);
    require(summary.hnr_window_count == 0 && !summary.hnr_db && !summary.hnr_std_db,
            "all-nil recording propagates nil mean and standard deviation");
    summary.windows.clear();
    pitchee::summarize_hnr(summary);
    require(summary.hnr_window_count == 0 && !summary.hnr_db && !summary.hnr_std_db,
            "empty recording propagates nil mean and standard deviation");
    std::cout << "PitcheeCore HNR test passed\n";
    return 0;
}

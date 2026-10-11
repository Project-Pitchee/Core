#include "pitchee/pitchee.h"
#include "pitchee/hnr.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Capture {
    size_t frames = 0;
    size_t voiced_frames = 0;
    double first_timestamp = 0.0;
    double last_timestamp = 0.0;
};

struct VadCapture {
    pitchee_vad_segment_t segments[16];
    size_t count = 0;
};

void capture_vad(
    const pitchee_vad_segment_t* segment,
    void* user_data
) {
    auto& capture = *static_cast<VadCapture*>(user_data);
    if (capture.count < std::size(capture.segments)) {
        capture.segments[capture.count++] = *segment;
    }
}

void capture_frame(
    const pitchee_f0_frame_t* frame,
    void* user_data
) {
    auto& capture = *static_cast<Capture*>(user_data);
    if (capture.frames == 0) capture.first_timestamp = frame->timestamp_seconds;
    capture.last_timestamp = frame->timestamp_seconds;
    capture.frames += 1;
    if (frame->voiced) capture.voiced_frames += 1;
}

pitchee_analyzer_t* create_analyzer(
    const char* model_directory,
    float min_f0_hz,
    char* error,
    size_t error_capacity
) {
    pitchee_analyzer_t* analyzer = nullptr;
    const auto status = pitchee_analyzer_create(
        model_directory,
        1,
        1,
        min_f0_hz,
        600.0f,
        0.9f,
        &analyzer,
        error,
        error_capacity
    );
    if (status == PITCHEE_ERROR_ORT_UNAVAILABLE) return nullptr;
    if (status != PITCHEE_SUCCESS) {
        std::cerr << "analyzer create failed: " << error << "\n";
        std::exit(1);
    }
    return analyzer;
}

Capture run_f0(
    pitchee_analyzer_t* analyzer,
    const std::vector<float>& samples,
    char* error,
    size_t error_capacity
) {
    pitchee_realtime_f0_t* stream = nullptr;
    if (pitchee_realtime_f0_create(
            analyzer,
            5120,
            256,
            &stream,
            error,
            error_capacity
        ) != PITCHEE_SUCCESS) {
        std::cerr << "F0 stream create failed: " << error << "\n";
        std::exit(1);
    }

    Capture capture;
    for (size_t start = 0; start < samples.size(); start += 256) {
        const size_t count = std::min<size_t>(256, samples.size() - start);
        if (pitchee_realtime_f0_process(
                stream,
                samples.data() + start,
                count,
                capture_frame,
                &capture,
                error,
                error_capacity
            ) != PITCHEE_SUCCESS) {
            std::cerr << "F0 stream process failed: " << error << "\n";
            pitchee_realtime_f0_destroy(stream);
            std::exit(1);
        }
    }
    pitchee_realtime_f0_destroy(stream);
    return capture;
}

void test_options(
    pitchee_analyzer_t* analyzer,
    const std::vector<float>& samples,
    char* error,
    size_t error_capacity
) {
    pitchee_realtime_f0_options_t options{};
    options.struct_size = sizeof(options);
    options.version = PITCHEE_REALTIME_F0_OPTIONS_VERSION;
    options.context_samples = 5120;
    options.inference_hop_samples = 256;
    options.output_rate_hz = 30.0;
    options.start_timestamp_seconds = 10.0;

    pitchee_realtime_f0_t* stream = nullptr;
    if (pitchee_realtime_f0_create_with_options(
            analyzer,
            &options,
            &stream,
            error,
            error_capacity
        ) != PITCHEE_SUCCESS) {
        std::cerr << "F0 stream options create failed: " << error << "\n";
        std::exit(1);
    }
    if (pitchee_realtime_f0_warmup(stream, error, error_capacity)
        != PITCHEE_SUCCESS) {
        std::cerr << "F0 warmup failed: " << error << "\n";
        std::exit(1);
    }

    pitchee_realtime_f0_metadata_t metadata{};
    metadata.struct_size = sizeof(metadata);
    metadata.version = PITCHEE_REALTIME_F0_METADATA_VERSION;
    if (pitchee_realtime_f0_get_metadata(
            stream,
            &metadata,
            error,
            error_capacity
        ) != PITCHEE_SUCCESS) {
        std::cerr << "F0 metadata failed: " << error << "\n";
        std::exit(1);
    }
    if (metadata.sample_rate != 16000
        || metadata.frame_hop_samples != 256
        || metadata.min_context_samples != 1024
        || std::abs(metadata.effective_output_rate_hz - 30.0) > 0.001) {
        std::cerr << "unexpected realtime F0 metadata\n";
        std::exit(1);
    }

    Capture capture;
    for (size_t start = 0; start < samples.size(); start += 256) {
        const size_t count = std::min<size_t>(256, samples.size() - start);
        if (pitchee_realtime_f0_process(
                stream,
                samples.data() + start,
                count,
                capture_frame,
                &capture,
                error,
                error_capacity
            ) != PITCHEE_SUCCESS) {
            std::cerr << "F0 options process failed: " << error << "\n";
            std::exit(1);
        }
    }

    pitchee_realtime_f0_stats_t stats{};
    stats.struct_size = sizeof(stats);
    stats.version = PITCHEE_REALTIME_F0_STATS_VERSION;
    if (pitchee_realtime_f0_get_stats(stream, &stats, error, error_capacity)
        != PITCHEE_SUCCESS) {
        std::cerr << "F0 stats failed: " << error << "\n";
        std::exit(1);
    }
    if (capture.frames == 0 || capture.first_timestamp < 10.0
        || capture.last_timestamp < capture.first_timestamp
        || stats.inference_calls == 0 || stats.emitted_frames == 0
        || stats.measured_output_hz < 27.0
        || stats.measured_output_hz > 33.0
        || stats.average_model_ms < 0.0 || stats.average_total_ms < 0.0) {
        std::cerr << "unexpected realtime F0 contract behavior\n";
        std::exit(1);
    }

    pitchee_realtime_f0_reset(stream);
    if (pitchee_realtime_f0_get_stats(stream, &stats, error, error_capacity)
        != PITCHEE_SUCCESS
        || stats.inference_calls != 0 || stats.emitted_frames != 0) {
        std::cerr << "realtime F0 reset did not clear stats\n";
        std::exit(1);
    }
    pitchee_realtime_f0_destroy(stream);
}

void test_vad_hnr_composition(
    pitchee_analyzer_t* analyzer,
    const std::vector<float>& samples,
    char* error,
    size_t error_capacity
) {
    VadCapture vad;
    if (pitchee_analyzer_detect_vad(
            analyzer,
            samples.data(),
            samples.size(),
            16000,
            1,
            capture_vad,
            &vad,
            error,
            error_capacity
        ) != PITCHEE_SUCCESS) {
        std::cerr << "VAD isolation failed: " << error << "\n";
        std::exit(1);
    }
    for (size_t index = 0; index < vad.count; ++index) {
        if (!(vad.segments[index].start_seconds < vad.segments[index].end_seconds)) {
            std::cerr << "VAD returned an invalid segment\n";
            std::exit(1);
        }
    }

    pitchee_hnr_summary_t summary{};
    if (pitchee_hnr_analyze(
            samples.data(),
            samples.size(),
            vad.segments,
            vad.count,
            nullptr,
            nullptr,
            &summary
        ) != PITCHEE_SUCCESS) {
        std::cerr << "HNR analysis failed\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "model directory is required\n";
        return 1;
    }

    char error[1024] = {};
    pitchee_analyzer_t* analyzer = create_analyzer(
        argv[1],
        75.0f,
        error,
        sizeof(error)
    );
    if (!analyzer) {
        std::cout << "PitcheeCore realtime F0 test skipped\n";
        return 0;
    }

    constexpr int sample_rate = 16000;
    constexpr int duration_samples = sample_rate * 2;
    std::vector<float> samples(duration_samples);
    for (int index = 0; index < duration_samples; ++index) {
        const double time = static_cast<double>(index) / sample_rate;
        samples[static_cast<size_t>(index)] = static_cast<float>(
            0.15 * std::sin(2.0 * kPi * 180.0 * time)
            + 0.05 * std::sin(2.0 * kPi * 360.0 * time)
        );
    }

    const Capture accepted = run_f0(
        analyzer,
        samples,
        error,
        sizeof(error)
    );
    if (accepted.frames == 0 || accepted.voiced_frames == 0) {
        std::cerr << "default SwiftF0 thresholds rejected the test signal\n";
        return 1;
    }
    test_options(analyzer, samples, error, sizeof(error));
    test_vad_hnr_composition(analyzer, samples, error, sizeof(error));
    pitchee_analyzer_destroy(analyzer);

    analyzer = create_analyzer(argv[1], 200.0f, error, sizeof(error));
    if (!analyzer) return 0;
    const Capture rejected = run_f0(
        analyzer,
        samples,
        error,
        sizeof(error)
    );
    pitchee_analyzer_destroy(analyzer);
    if (rejected.frames == 0 || rejected.voiced_frames != 0) {
        std::cerr << "analyzer SwiftF0 thresholds were not applied\n";
        return 1;
    }

    std::cout << "PitcheeCore realtime F0 test passed\n";
    return 0;
}

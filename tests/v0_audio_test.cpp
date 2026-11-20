#include "v0/v0_audio.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

void require_close(float actual, float expected, float tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "FAILED: " << message << " (" << actual << " != " << expected
                  << ")\n";
        std::exit(1);
    }
}

void require_close(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << "FAILED: " << message << " (" << actual << " != " << expected
                  << ")\n";
        std::exit(1);
    }
}

std::vector<float> sine(
    int sample_rate,
    double seconds,
    double frequency,
    double amplitude = 1.0
) {
    const int count = static_cast<int>(sample_rate * seconds);
    std::vector<float> values(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        values[static_cast<size_t>(index)] = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * frequency * index / sample_rate)
        );
    }
    return values;
}

void test_resampling() {
    const std::vector<float> sine_48k = sine(48000, 1.0, 440.0);
    const std::vector<float> output_48k = pitchee::v0::resample_poly(
        sine_48k.data(), sine_48k.size(), 48000, 16000
    );
    require(output_48k.size() == 16000, "48 kHz output length");
    const float expected_48k[] = {
        0.015687337f,
        0.169638812f,
        0.339472234f,
        0.495117813f,
        0.637879074f,
    };
    for (size_t index = 0; index < std::size(expected_48k); ++index) {
        require_close(output_48k[index], expected_48k[index], 2e-6f, "48 kHz sample");
    }

    const std::vector<float> sine_44k = sine(44100, 1.0, 440.0);
    const std::vector<float> output_44k = pitchee::v0::resample_poly(
        sine_44k.data(), sine_44k.size(), 44100, 16000
    );
    require(output_44k.size() == 16000, "44.1 kHz output length");
    const float expected_44k[] = {
        0.015325366f,
        0.169617191f,
        0.339486569f,
        0.495127946f,
        0.637885988f,
    };
    for (size_t index = 0; index < std::size(expected_44k); ++index) {
        require_close(output_44k[index], expected_44k[index], 2e-6f, "44.1 kHz sample");
    }
}

void test_channel_mix_and_normalization() {
    const float stereo[] = {0.25f, 0.75f, -0.25f, -0.75f};
    const auto mono = pitchee::v0::average_channels(stereo, 4, 2);
    require(mono.size() == 2, "stereo output length");
    require(mono[0] == 0.5f && mono[1] == -0.5f, "stereo average");

    const auto normalized = pitchee::v0::normalize_waveform(mono);
    require(normalized.size() == mono.size(), "normalized length");
    require_close(normalized[0], 1.0f, 1e-5f, "normalized positive sample");
    require_close(normalized[1], -1.0f, 1e-5f, "normalized negative sample");
}

void test_short_audio_padding() {
    const std::vector<float> short_signal(1000, 0.25f);
    const auto processed = pitchee::v0::prepare_audio(
        short_signal.data(), short_signal.size(), 16000, 1
    );
    require(processed.metadata.source_seconds == 1000.0 / 16000.0,
            "short source duration");
    require(processed.metadata.analyzed_seconds == 0.4, "short audio minimum");
    require(processed.samples.size() == 6400, "short audio padded to 6400 samples");
}

void test_long_silence_is_still_capped() {
    const std::vector<float> silence(60 * 16000, 0.0f);
    const auto processed = pitchee::v0::prepare_audio(
        silence.data(), silence.size(), 16000, 1
    );
    require(processed.metadata.analyzed_seconds == 12.0,
            "silence still respects the 12 second cap");
    require_close(processed.metadata.speech_seconds, 59.99, 1e-9,
                  "silence active-frame metadata matches vf3.audio");
}

void test_exact_twelve_seconds_is_not_shortened() {
    constexpr int sample_rate = 16000;
    const std::vector<float> signal = sine(sample_rate, 12.0, 220.0, 0.3);
    const auto processed = pitchee::v0::prepare_audio(
        signal.data(), signal.size(), sample_rate, 1
    );
    require(processed.metadata.source_seconds == 12.0, "exact source duration");
    require(processed.metadata.analyzed_seconds == 12.0, "exact window duration");
    require(processed.samples.size() == 12 * sample_rate, "exact window samples");
}

void test_twelve_second_window() {
    constexpr int sample_rate = 16000;
    std::vector<float> signal(60 * sample_rate, 0.0f);
    const std::vector<float> intro = sine(sample_rate, 0.3, 180.0, 0.05);
    for (size_t index = 0; index < intro.size(); ++index) {
        signal[static_cast<size_t>(sample_rate) + index] = intro[index];
    }
    const std::vector<float> speech = sine(sample_rate, 14.0, 220.0, 0.3);
    for (size_t index = 0; index < speech.size(); ++index) {
        signal[static_cast<size_t>(30 * sample_rate) + index] = speech[index];
    }

    const auto processed = pitchee::v0::prepare_audio(
        signal.data(), signal.size(), sample_rate, 1
    );
    require(processed.metadata.source_seconds == 60.0, "long source duration");
    require(processed.metadata.analyzed_seconds == 12.0, "long audio window length");
    require(processed.samples.size() == 12 * sample_rate, "long audio window samples");
    require(processed.metadata.active_frames >= 1430,
            "long audio active frame count");
    require(processed.metadata.crop_start_sample >= 476000
                && processed.metadata.crop_start_sample <= 479000,
            "speech-dense crop start");
}

}  // namespace

int main() {
    test_resampling();
    test_channel_mix_and_normalization();
    test_short_audio_padding();
    test_long_silence_is_still_capped();
    test_exact_twelve_seconds_is_not_shortened();
    test_twelve_second_window();
    std::cout << "PitcheeCore V0 audio test passed\n";
    return 0;
}

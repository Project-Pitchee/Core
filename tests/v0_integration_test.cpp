#include "pitchee/v0.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kLongSampleRate = 48000;
constexpr int kLongChannels = 2;
constexpr double kLongSeconds = 20.0;

int fail(const char* message) {
    std::fprintf(stderr, "FAILED: %s\n", message);
    return 1;
}

void put_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void put_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
    }
}

bool write_pcm16_wav(
    const std::filesystem::path& path,
    const std::vector<float>& interleaved,
    int sample_rate,
    int channels
) {
    const uint32_t byte_count = static_cast<uint32_t>(interleaved.size() * 2);
    std::vector<uint8_t> header;
    header.reserve(44);
    header.insert(header.end(), {'R', 'I', 'F', 'F'});
    put_u32(header, 36 + byte_count);
    header.insert(header.end(), {'W', 'A', 'V', 'E'});
    header.insert(header.end(), {'f', 'm', 't', ' '});
    put_u32(header, 16);
    put_u16(header, 1);
    put_u16(header, static_cast<uint16_t>(channels));
    put_u32(header, static_cast<uint32_t>(sample_rate));
    put_u32(header, static_cast<uint32_t>(sample_rate * channels * 2));
    put_u16(header, static_cast<uint16_t>(channels * 2));
    put_u16(header, 16);
    header.insert(header.end(), {'d', 'a', 't', 'a'});
    put_u32(header, byte_count);

    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    for (float sample : interleaved) {
        const float clipped = std::clamp(sample, -1.0f, 1.0f);
        const int16_t value = static_cast<int16_t>(std::lround(clipped * 32767.0f));
        const char encoded[2] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff),
        };
        output.write(encoded, sizeof(encoded));
    }
    return static_cast<bool>(output);
}

std::vector<float> make_long_stereo_signal() {
    const int samples = static_cast<int>(kLongSampleRate * kLongSeconds);
    std::vector<float> signal(static_cast<size_t>(samples * kLongChannels));
    for (int index = 0; index < samples; ++index) {
        const double time = static_cast<double>(index) / kLongSampleRate;
        if (time < 2.0 || time >= 18.0) continue;
        const double local = time - 2.0;
        const double envelope = 0.12 + 0.08 * std::sin(2.0 * kPi * 2.7 * local);
        const double voiced = std::sin(2.0 * kPi * 185.0 * local)
            + 0.45 * std::sin(2.0 * kPi * 370.0 * local)
            + 0.20 * std::sin(2.0 * kPi * 555.0 * local);
        const double value = envelope * voiced;
        signal[static_cast<size_t>(index * kLongChannels)] = static_cast<float>(value);
        signal[static_cast<size_t>(index * kLongChannels + 1)] = static_cast<float>(value * 0.8);
    }
    return signal;
}

bool valid_result(const pitchee_v0_result_t& result) {
    if (result.predicted_class < PITCHEE_V0_CLASS_MALE_NATURAL
        || result.predicted_class > PITCHEE_V0_CLASS_FEMALE_NATURAL) {
        return false;
    }
    double sum = 0.0;
    for (double probability : result.probabilities) {
        if (!std::isfinite(probability) || probability < 0.0 || probability > 1.0) {
            return false;
        }
        sum += probability;
    }
    if (std::abs(sum - 1.0) > 1e-6) return false;
    if (!std::isfinite(result.confidence) || !std::isfinite(result.timbre_score)
        || !std::isfinite(result.timbre_base) || !std::isfinite(result.timbre_cap)
        || !std::isfinite(result.naturalness_score)) {
        return false;
    }
    if (result.confidence < 0.0 || result.confidence > 1.0) return false;
    if (result.timbre_score < 1.0 || result.timbre_score > 99.0) return false;
    if (result.naturalness_score < 1.0 || result.naturalness_score > 99.0) return false;
    if (result.timbre_score > result.timbre_cap + 1e-9) return false;
    if (result.confidence < 0.85 && result.timbre_score > 49.9 + 1e-9) return false;
    if ((result.passed != 0) != (result.timbre_score >= 50.0)) return false;
    return true;
}

bool rejects_corrupt_model() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory = std::filesystem::temp_directory_path()
        / ("pitchee_v0_corrupt_" + std::to_string(nonce));
    std::error_code ignored;
    std::filesystem::create_directories(directory, ignored);
    std::ofstream output(directory / PITCHEE_V0_DEFAULT_MODEL, std::ios::binary);
    if (!output) return false;
    output << "not the V0 model";
    char error[1024] = {};
    pitchee_v0_t* core = nullptr;
    const pitchee_status_t status = pitchee_v0_create(
        directory.string().c_str(), 1, 0, &core, error, sizeof(error)
    );
    pitchee_v0_destroy(core);
    std::filesystem::remove_all(directory, ignored);
    if (status != PITCHEE_ERROR_MODEL) {
        std::fprintf(stderr, "corrupt model status: %d (%s)\n",
                     static_cast<int>(status), error);
        return false;
    }
    return std::string(error).find("SHA-256 mismatch") != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: pitchee_v0_integration_test <model-directory>\n");
        return 2;
    }

    if (!rejects_corrupt_model()) return fail("corrupt model rejection");

    char error[1024] = {};
    pitchee_v0_t* core = nullptr;
    pitchee_status_t status = pitchee_v0_create(
        argv[1], 2, 0, &core, error, sizeof(error)
    );
    if (status == PITCHEE_ERROR_ORT_UNAVAILABLE) {
        std::fprintf(stdout, "SKIP: ONNX Runtime backend unavailable\n");
        return 77;
    }
    if (status != PITCHEE_SUCCESS) {
        std::fprintf(stderr, "create failed: %s\n", error);
        return fail("V0 create");
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path wav_path = std::filesystem::temp_directory_path()
        / ("pitchee_v0_integration_" + std::to_string(nonce) + ".wav");
    const std::vector<float> long_signal = make_long_stereo_signal();
    if (!write_pcm16_wav(
            wav_path, long_signal, kLongSampleRate, kLongChannels
        )) {
        pitchee_v0_destroy(core);
        return fail("write WAV fixture");
    }

    pitchee_v0_result_t result{};
    result.struct_size = sizeof(result);
    result.version = PITCHEE_V0_RESULT_VERSION;
    status = pitchee_v0_analyze_wav_file(
        core, wav_path.string().c_str(), &result, error, sizeof(error)
    );
    std::error_code ignored;
    std::filesystem::remove(wav_path, ignored);
    if (status != PITCHEE_SUCCESS) {
        std::fprintf(stderr, "long analysis failed: %s\n", error);
        pitchee_v0_destroy(core);
        return fail("20 second WAV analysis");
    }
    if (std::abs(result.source_seconds - 20.0) > 1e-6) {
        pitchee_v0_destroy(core);
        return fail("20 second source duration");
    }
    if (std::abs(result.analyzed_seconds - 12.0) > 1e-9) {
        pitchee_v0_destroy(core);
        return fail("long input must analyze exactly 12 seconds");
    }
    if (result.speech_seconds <= 0.0 || result.speech_seconds > 20.0) {
        pitchee_v0_destroy(core);
        return fail("speech duration");
    }
    if (!valid_result(result)) {
        pitchee_v0_destroy(core);
        return fail("long result invariants");
    }

    constexpr int short_rate = 16000;
    std::vector<float> short_pcm(short_rate / 10);
    for (size_t index = 0; index < short_pcm.size(); ++index) {
        short_pcm[index] = static_cast<float>(
            0.2 * std::sin(2.0 * kPi * 210.0 * static_cast<double>(index) / short_rate)
        );
    }
    pitchee_v0_result_t short_result{};
    short_result.struct_size = sizeof(short_result);
    short_result.version = PITCHEE_V0_RESULT_VERSION;
    status = pitchee_v0_analyze_pcm(
        core,
        short_pcm.data(),
        short_pcm.size(),
        short_rate,
        1,
        &short_result,
        error,
        sizeof(error)
    );
    pitchee_v0_destroy(core);
    if (status != PITCHEE_SUCCESS) {
        std::fprintf(stderr, "short analysis failed: %s\n", error);
        return fail("short PCM analysis");
    }
    if (std::abs(short_result.source_seconds - 0.1) > 1e-6
        || std::abs(short_result.analyzed_seconds - 0.4) > 1e-9) {
        return fail("short input minimum duration");
    }
    if (!valid_result(short_result)) return fail("short result invariants");

    std::printf(
        "PitcheeCore V0 integration test passed: 20s -> %.1fs analyzed, 0.1s -> %.1fs analyzed\n",
        result.analyzed_seconds,
        short_result.analyzed_seconds
    );
    return 0;
}

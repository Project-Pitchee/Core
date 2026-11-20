#include "wav_reader.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace pitchee {
namespace {

uint16_t read_u16(const unsigned char* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t read_u32(const unsigned char* data) {
    return static_cast<uint32_t>(data[0])
        | (static_cast<uint32_t>(data[1]) << 8)
        | (static_cast<uint32_t>(data[2]) << 16)
        | (static_cast<uint32_t>(data[3]) << 24);
}

}  // namespace

WavData read_wav(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open WAV: " + path.string());
    std::vector<unsigned char> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("invalid RIFF/WAVE file");
    }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    const unsigned char* pcm = nullptr;
    size_t pcm_size = 0;

    size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const unsigned char* chunk = bytes.data() + offset;
        const uint32_t chunk_size = read_u32(chunk + 4);
        const size_t payload = offset + 8;
        if (payload + chunk_size > bytes.size()) break;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format = read_u16(bytes.data() + payload);
            channels = read_u16(bytes.data() + payload + 2);
            sample_rate = read_u32(bytes.data() + payload + 4);
            bits_per_sample = read_u16(bytes.data() + payload + 14);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            pcm = bytes.data() + payload;
            pcm_size = chunk_size;
        }
        offset = payload + chunk_size + (chunk_size & 1u);
    }

    if (!pcm || channels == 0 || sample_rate == 0) {
        throw std::runtime_error("WAV has no PCM data");
    }
    WavData result;
    result.sample_rate = static_cast<int>(sample_rate);
    result.channels = static_cast<int>(channels);

    if (audio_format == 3 && bits_per_sample == 32) {
        const size_t count = pcm_size / sizeof(float);
        result.samples.resize(count);
        std::memcpy(result.samples.data(), pcm, count * sizeof(float));
    } else if (audio_format == 1 && bits_per_sample == 16) {
        const size_t count = pcm_size / sizeof(int16_t);
        result.samples.resize(count);
        for (size_t index = 0; index < count; ++index) {
            int16_t value = 0;
            std::memcpy(&value, pcm + index * sizeof(int16_t), sizeof(int16_t));
            result.samples[index] = static_cast<float>(value) / 32768.0f;
        }
    } else if (audio_format == 1 && bits_per_sample == 32) {
        const size_t count = pcm_size / sizeof(int32_t);
        result.samples.resize(count);
        for (size_t index = 0; index < count; ++index) {
            int32_t value = 0;
            std::memcpy(&value, pcm + index * sizeof(int32_t), sizeof(int32_t));
            result.samples[index] = static_cast<float>(
                static_cast<double>(value) / 2147483648.0
            );
        }
    } else {
        throw std::runtime_error(
            "unsupported WAV format; use PCM16, PCM32, or Float32"
        );
    }
    return result;
}

}  // namespace pitchee

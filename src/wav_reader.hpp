#ifndef PITCHEE_WAV_READER_HPP
#define PITCHEE_WAV_READER_HPP

#include <filesystem>
#include <vector>

namespace pitchee {

struct WavData {
    std::vector<float> samples;
    int sample_rate = 0;
    int channels = 0;
};

WavData read_wav(const std::filesystem::path& path);

}  // namespace pitchee

#endif

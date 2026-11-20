#ifndef PITCHEE_SHA256_HPP
#define PITCHEE_SHA256_HPP

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace pitchee {

std::array<uint8_t, 32> sha256_file(const std::filesystem::path& path);

std::string sha256_hex(const std::array<uint8_t, 32>& digest);

}  // namespace pitchee

#endif

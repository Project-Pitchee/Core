#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace pitchee {
namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

uint32_t rotate_right(uint32_t value, uint32_t count) {
    return (value >> count) | (value << (32u - count));
}

void compress(std::array<uint32_t, 8>& state, const uint8_t* block) {
    std::array<uint32_t, 64> schedule{};
    for (size_t index = 0; index < 16; ++index) {
        const size_t offset = index * 4;
        schedule[index] = (static_cast<uint32_t>(block[offset]) << 24)
            | (static_cast<uint32_t>(block[offset + 1]) << 16)
            | (static_cast<uint32_t>(block[offset + 2]) << 8)
            | static_cast<uint32_t>(block[offset + 3]);
    }
    for (size_t index = 16; index < 64; ++index) {
        const uint32_t s0 = rotate_right(schedule[index - 15], 7)
            ^ rotate_right(schedule[index - 15], 18)
            ^ (schedule[index - 15] >> 3);
        const uint32_t s1 = rotate_right(schedule[index - 2], 17)
            ^ rotate_right(schedule[index - 2], 19)
            ^ (schedule[index - 2] >> 10);
        schedule[index] = schedule[index - 16] + s0
            + schedule[index - 7] + s1;
    }

    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    uint32_t e = state[4];
    uint32_t f = state[5];
    uint32_t g = state[6];
    uint32_t h = state[7];
    for (size_t index = 0; index < 64; ++index) {
        const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + choose + kRoundConstants[index] + schedule[index];
        const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

std::array<uint8_t, 32> sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("model SHA-256 read failed: " + path.string());
    }

    std::array<uint32_t, 8> state = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    std::array<uint8_t, 64> block{};
    uint64_t total_bytes = 0;
    size_t tail_bytes = 0;
    for (;;) {
        input.read(
            reinterpret_cast<char*>(block.data() + tail_bytes),
            static_cast<std::streamsize>(block.size() - tail_bytes)
        );
        const size_t count = static_cast<size_t>(input.gcount());
        total_bytes += static_cast<uint64_t>(count);
        tail_bytes += count;
        if (tail_bytes == block.size()) {
            compress(state, block.data());
            tail_bytes = 0;
            continue;
        }
        break;
    }
    if (input.bad()) {
        throw std::runtime_error("model SHA-256 read failed: " + path.string());
    }
    std::fill(block.begin() + static_cast<std::ptrdiff_t>(tail_bytes), block.end(), 0);
    block[tail_bytes] = 0x80;
    if (tail_bytes >= 56) {
        compress(state, block.data());
        block.fill(0);
    }
    const uint64_t bit_length = total_bytes * 8u;
    for (size_t index = 0; index < 8; ++index) {
        block[63 - index] = static_cast<uint8_t>(
            (bit_length >> (index * 8u)) & 0xffu
        );
    }
    compress(state, block.data());

    std::array<uint8_t, 32> digest{};
    for (size_t word = 0; word < state.size(); ++word) {
        for (size_t byte = 0; byte < 4; ++byte) {
            digest[word * 4 + byte] = static_cast<uint8_t>(
                (state[word] >> (24u - byte * 8u)) & 0xffu
            );
        }
    }
    return digest;
}

std::string sha256_hex(const std::array<uint8_t, 32>& digest) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string text(64, '0');
    for (size_t index = 0; index < digest.size(); ++index) {
        text[index * 2] = kHex[digest[index] >> 4];
        text[index * 2 + 1] = kHex[digest[index] & 0x0fu];
    }
    return text;
}

}  // namespace pitchee

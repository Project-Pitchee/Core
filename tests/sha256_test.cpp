#include "sha256.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace {

bool require_digest(const std::string& contents, const char* expected) {
    const auto nonce = std::to_string(
        std::hash<std::string>{}(contents) ^ contents.size()
    );
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("pitchee_sha256_" + nonce + ".bin");
    {
        std::ofstream output(path, std::ios::binary);
        if (!output) return false;
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!output) return false;
    }
    const std::string actual = pitchee::sha256_hex(pitchee::sha256_file(path));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    if (actual != expected) {
        std::fprintf(stderr, "SHA-256 mismatch: expected %s, got %s\n",
                     expected, actual.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!require_digest(
            "",
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        )) return 1;
    if (!require_digest(
            "abc",
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        )) return 1;
    if (!require_digest(
            std::string(55, 'a'),
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"
        )) return 1;
    if (!require_digest(
            std::string(56, 'a'),
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"
        )) return 1;
    if (!require_digest(
            std::string(64, 'a'),
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"
        )) return 1;
    if (!require_digest(
            std::string(65, 'a'),
            "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"
        )) return 1;
    if (!require_digest(
            std::string(1000000, 'a'),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
        )) return 1;
    std::printf("PitcheeCore SHA-256 test passed\n");
    return 0;
}

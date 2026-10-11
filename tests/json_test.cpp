#include "internal.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

bool contains(const std::string& value, const std::string& part) {
    return value.find(part) != std::string::npos;
}

}  // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path()
        / ("pitchee-manifest-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()
        ));
    std::filesystem::create_directories(directory);
    require(pitchee::model_version_from_directory(directory) == "unknown",
            "missing manifest must not report a bundled version");
    for (const std::string contents : {
        R"({"modelVersion":null,"files":{}})",
        R"({"modelVersion":"","files":{}})",
        R"({"files":{}})",
        R"({"modelVersion":"unfinished})"
    }) {
        std::ofstream(directory / "manifest.json") << contents;
        require(pitchee::model_version_from_directory(directory) == "unknown",
                "invalid version must not consume another field");
    }
    std::ofstream(directory / "manifest.json")
        << "{\n  \"modelVersion\" : \"test-2.3+local\",\n  \"files\": {}\n}";
    pitchee::AnalysisResult result;
    result.model_version = pitchee::model_version_from_directory(directory);
    std::filesystem::remove_all(directory);
    result.vfp_standard_score = 12.5;
    result.window_count = 1;
    result.window_duration_seconds = 1.5;
    result.vfp_windows.push_back({0.0, 1.5, 12.5});
    result.naturalness_score = 67.5;
    result.naturalness_window_duration_seconds = 1.5;
    result.naturalness_windows.push_back({0.0, 1.5, 70.0});
    result.voiced_frame_count = 20;
    result.voiced_window_count = 2;
    result.f0_windows.push_back({0.0, 0.1, false, 0.0});
    result.f0_windows.push_back({0.1, 0.2, true, 180.0});

    const std::string json = pitchee::result_to_json(result);
    require(contains(json, "\"schema_version\":3"), "schema version");
    require(contains(json, "\"model_version\":\"test-2.3+local\""),
            "result version must come from the selected model directory");
    require(contains(json, "\"score_profile\":\"feminization\""),
            "score profile");
    require(!contains(json, "\"models\""), "models removed");
    require(!contains(json, "raw_female_score"), "raw score removed");
    require(contains(json, "\"f0\":{\"window_seconds\":0.05"), "f0 section");
    require(!contains(json, "\"voice_quality\""), "HNR must remain external to analysis JSON");
    require(contains(json, "\"vfp\":{\"vfp_standard_score\":12.5"), "vfp section");
    require(
        contains(json, "\"naturalness\":{\"score\":67.5"),
        "naturalness section"
    );
    require(
        contains(json, "\"windows\":[{\"start_seconds\":0,\"end_seconds\":1.5,\"score\":70}"),
        "naturalness windows"
    );
    require(
        contains(json, "\"vfp_standard_score\":12.5"),
        "window vfp score"
    );
    result.model_version = "test\"\\\n";
    require(contains(pitchee::result_to_json(result),
                     "\"model_version\":\"test\\\"\\\\\\n\""),
            "model version must be JSON escaped");
    result.score_profile = 1;
    require(contains(pitchee::result_to_json(result),
                     "\"score_profile\":\"masculinization\""),
            "masculinization score profile");
    std::cout << "PitcheeCore JSON test passed\n";
    return 0;
}

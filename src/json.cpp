#include "internal.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <regex>
#include <sstream>

namespace pitchee {
namespace {

std::string number(double value) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

std::string optional_number(bool present, double value) {
    return present ? number(value) : "null";
}

const char* score_profile_name(int profile) {
    return profile == 1
        ? "masculinization"
        : "feminization";
}

}  // namespace

std::string model_version_from_directory(const std::filesystem::path& directory) {
    std::ifstream manifest(directory / "manifest.json");
    if (!manifest) return "unknown";
    const std::string contents{
        std::istreambuf_iterator<char>(manifest),
        std::istreambuf_iterator<char>()
    };
    // Manifest versions are plain version tags. Missing or invalid metadata
    // must not be mistaken for a following field or a bundled model version.
    static const std::regex pattern(
        R"version("modelVersion"\s*:\s*"([A-Za-z0-9][A-Za-z0-9._+-]*)"\s*[,}])version"
    );
    std::smatch match;
    return std::regex_search(contents, match, pattern) ? match[1].str() : "unknown";
}

std::string escape_json(const std::string& value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (character < 0x20) {
                    std::ostringstream escaped;
                    escaped << "\\u" << std::hex << std::setw(4)
                            << std::setfill('0') << static_cast<int>(character);
                    output += escaped.str();
                } else {
                    output.push_back(static_cast<char>(character));
                }
        }
    }
    return output;
}

std::string result_to_json(const AnalysisResult& result) {
    std::ostringstream output;
    output << "{";
    output << "\"schema_version\":" << kSchemaVersion << ",";
    output << "\"model_version\":\""
           << escape_json(result.model_version) << "\",";
    output << "\"score_profile\":\""
           << score_profile_name(result.score_profile) << "\",";

    output << "\"audio\":{";
    output << "\"source_sample_rate\":" << result.source_sample_rate << ",";
    output << "\"source_channels\":" << result.source_channels << ",";
    output << "\"input_seconds\":" << number(result.source_seconds) << ",";
    output << "\"analyzed_seconds\":" << number(result.analyzed_seconds);
    output << "},";

    output << "\"vad\":{";
    output << "\"segment_count\":" << result.vad.segments.size() << ",";
    output << "\"speech_seconds\":" << number(result.vad.speech_seconds) << ",";
    output << "\"silero_segment_count\":" << result.vad.silero_segment_count << ",";
    output << "\"discarded_breath_like_count\":"
           << result.vad.discarded_breath_like_count << ",";
    output << "\"trimmed_segment_count\":" << result.vad.trimmed_segment_count << ",";
    output << "\"segments\":[";
    for (size_t index = 0; index < result.vad.segments.size(); ++index) {
        const auto& segment = result.vad.segments[index];
        if (index) output << ",";
        output << "{\"start_seconds\":" << number(segment.source_start_seconds)
               << ",\"end_seconds\":" << number(segment.source_end_seconds)
               << ",\"speech_start_seconds\":" << number(segment.speech_start_seconds)
               << ",\"speech_end_seconds\":" << number(segment.speech_end_seconds)
               << "}";
    }
    output << "]},";

    output << "\"f0\":{";
    output << "\"window_seconds\":0.05,";
    output << "\"mean_hz\":" << optional_number(result.has_f0, result.f0_mean_hz) << ",";
    output << "\"standard_deviation_hz\":"
           << optional_number(result.has_f0, result.f0_standard_deviation_hz) << ",";
    output << "\"voiced_frame_count\":" << result.voiced_frame_count << ",";
    output << "\"voiced_window_count\":" << result.voiced_window_count << ",";
    output << "\"windows\":[";
    for (size_t index = 0; index < result.f0_windows.size(); ++index) {
        const auto& window = result.f0_windows[index];
        if (index) output << ",";
        output << "{\"start_seconds\":" << number(window.start_seconds)
               << ",\"end_seconds\":" << number(window.end_seconds)
               << ",\"f0_hz\":"
               << optional_number(window.has_f0, window.f0_hz)
               << "}";
    }
    output << "]},";

    output << "\"vfp\":{";
    output << "\"vfp_standard_score\":" << number(result.vfp_standard_score) << ",";
    output << "\"window_count\":" << result.window_count << ",";
    output << "\"window_duration_seconds\":"
           << number(result.window_duration_seconds) << ",";
    output << "\"windows\":[";
    for (size_t index = 0; index < result.vfp_windows.size(); ++index) {
        const auto& window = result.vfp_windows[index];
        if (index) output << ",";
        output << "{\"start_seconds\":" << number(window.start_seconds)
               << ",\"end_seconds\":" << number(window.end_seconds)
               << ",\"vfp_standard_score\":"
               << number(window.vfp_standard_score)
               << "}";
    }
    output << "]},";

    output << "\"naturalness\":{";
    output << "\"score\":" << number(result.naturalness_score) << ",";
    output << "\"window_count\":" << result.naturalness_windows.size() << ",";
    output << "\"window_duration_seconds\":"
           << number(result.naturalness_window_duration_seconds) << ",";
    output << "\"windows\":[";
    for (size_t index = 0; index < result.naturalness_windows.size(); ++index) {
        const auto& window = result.naturalness_windows[index];
        if (index) output << ",";
        output << "{\"start_seconds\":" << number(window.start_seconds)
               << ",\"end_seconds\":" << number(window.end_seconds)
               << ",\"score\":" << number(window.score)
               << "}";
    }
    output << "]},";

    output << "\"composite\":{";
    output << "\"base_score\":" << number(result.score.base_score) << ",";
    output << "\"final_score\":" << number(result.score.final_score) << ",";
    output << "\"cap\":"
           << optional_number(result.score.has_score_cap, result.score.score_cap) << ",";
    output << "\"rule\":\"" << escape_json(result.score.score_rule) << "\",";
    output << "\"limited\":" << (result.score.score_limited ? "true" : "false") << ",";
    output << "\"boosted\":" << (result.score.score_boosted ? "true" : "false");
    output << "}";
    output << "}";
    return output.str();
}

}  // namespace pitchee

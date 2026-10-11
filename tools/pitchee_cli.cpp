#include "pitchee/pitchee.h"

#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#ifdef __APPLE__
#include "mac_recorder.hpp"
#endif

namespace {

void progress_callback(const pitchee_progress_t* progress, void*) {
    std::fprintf(
        stderr,
        "[pitchee-progress] stage=%d completed=%" PRIu64
        " total=%" PRIu64 " fraction=%.6f\n",
        static_cast<int>(progress->stage),
        progress->completed,
        progress->total,
        progress->fraction
    );
}

bool parse_profile(const char* value, pitchee_score_profile_t& profile) {
    if (std::strcmp(value, "feminization") == 0) {
        profile = PITCHEE_SCORE_PROFILE_FEMINIZATION;
        return true;
    }
    if (std::strcmp(value, "masculinization") == 0) {
        profile = PITCHEE_SCORE_PROFILE_MASCULINIZATION;
        return true;
    }
    return false;
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') quoted += "'\\''";
        else quoted.push_back(character);
    }
    quoted.push_back('\'');
    return quoted;
}

bool json_number(const std::string& json, const std::string& key, double& value) {
    const size_t position = json.find(key);
    if (position == std::string::npos) return false;
    const char* start = json.c_str() + position + key.size();
    char* end = nullptr;
    value = std::strtod(start, &end);
    return end != start;
}

std::string json_string(const std::string& json, const std::string& key) {
    const size_t position = json.find(key);
    if (position == std::string::npos) return {};
    const size_t start = position + key.size();
    const size_t end = json.find('"', start);
    return end == std::string::npos ? std::string{} : json.substr(start, end - start);
}

}  // namespace

int main(int argc, char** argv) {
    const bool record_mode = argc >= 5 && std::strcmp(argv[2], "record") == 0;
    if ((!record_mode && argc != 4) || (record_mode && argc != 5 && argc != 6)) {
        std::cerr << "usage: pitchee_cli <model-directory> <audio.wav> "
                     "<feminization|masculinization>\n"
                     "       pitchee_cli <model-directory> record <seconds> "
                     "<feminization|masculinization> [output.wav]\n";
        return 2;
    }

    pitchee_score_profile_t score_profile;
    const char* profile_argument = record_mode ? argv[4] : argv[3];
    if (!parse_profile(profile_argument, score_profile)) {
        std::cerr << "invalid score profile: " << profile_argument << "\n";
        return 2;
    }

    std::string audio_path = record_mode ? std::string{} : argv[2];
    if (record_mode) {
        char* duration_end = nullptr;
        const double duration = std::strtod(argv[3], &duration_end);
        if (duration_end == argv[3] || *duration_end != '\0'
            || duration <= 0.0 || duration > 60.0) {
            std::cerr << "recording duration must be between 0 and 60 seconds\n";
            return 2;
        }
        if (argc == 6) {
            audio_path = argv[5];
        } else {
            audio_path = (
                std::filesystem::temp_directory_path()
                / "pitchee-cli-recording.wav"
            ).string();
        }
        const char* configured_device = std::getenv("PITCHEE_MAC_AUDIO_DEVICE");
        const std::string device = configured_device && *configured_device
            ? configured_device
            : ":2";
        std::string device_index;
        if (!device.empty() && device.front() == ':') {
            device_index = device.substr(1);
        } else if (!device.empty()
                   && device.find_first_not_of("0123456789") == std::string::npos) {
            device_index = device;
        }
        std::cerr << "Recording " << duration << " seconds to " << audio_path
                  << " from the default macOS input...\n";
#ifdef __APPLE__
        std::string recording_error;
        if (!record_macos_audio(audio_path, duration, recording_error)) {
            std::cerr << "macOS recording failed: " << recording_error << "\n";
            return 1;
        }
#else
        std::ostringstream command;
        command << "ffmpeg -hide_banner -loglevel error -nostdin"
                << " -thread_queue_size 2048 -f avfoundation";
        if (!device_index.empty()) {
            command << " -audio_device_index " << device_index;
        }
        command << " -i " << shell_quote(device_index.empty() ? device : "")
                << " -t " << duration
                << " -ac 1 -ar 48000 -c:a pcm_f32le -y "
                << shell_quote(audio_path);
        const int result = std::system(command.str().c_str());
        if (result != 0) {
            std::cerr << "ffmpeg recording failed with exit code " << result
                      << ". Set PITCHEE_MAC_AUDIO_DEVICE if the input index changed.\n";
            return 1;
        }
#endif
    }

    char error[1024] = {};
    pitchee_analyzer_t* analyzer = nullptr;
    pitchee_status_t status = pitchee_analyzer_create(
        argv[1],
        2,
        1,
        75.0f,
        600.0f,
        0.9f,
        &analyzer,
        error,
        sizeof(error)
    );
    if (status != PITCHEE_SUCCESS) {
        std::cerr << "analyzer_create failed: " << error << "\n";
        return static_cast<int>(status);
    }

    char* json = nullptr;
    const char* show_progress = std::getenv("PITCHEE_CLI_PROGRESS");
    status = pitchee_analyze_wav_file(
        analyzer,
        audio_path.c_str(),
        score_profile,
        show_progress && *show_progress ? progress_callback : nullptr,
        nullptr,
        &json,
        error,
        sizeof(error)
    );
    if (status != PITCHEE_SUCCESS) {
        std::cerr << "analysis failed: " << error << "\n";
        pitchee_analyzer_destroy(analyzer);
        return static_cast<int>(status);
    }
    if (record_mode) {
        double health_score = 0.0;
        double disorder_probability = 0.0;
        double f0_mean = 0.0;
        double composite = 0.0;
        json_number(json, "\"voice_health\":{\"score\":", health_score);
        json_number(json, "\"disorder_probability\":", disorder_probability);
        json_number(json, "\"mean_hz\":", f0_mean);
        json_number(json, "\"final_score\":", composite);
        const std::string health_label = json_string(json, "\"label\":\"");
        std::cout << "recording: " << audio_path << "\n";
        std::cout << "voice_health: " << health_score << "/100 (" << health_label << ")\n";
        std::cout << "disorder_probability: " << disorder_probability << "%\n";
        std::cout << "f0_mean_hz: " << f0_mean << "\n";
        std::cout << "composite_score: " << composite << "\n";
    } else {
        std::cout << json << "\n";
    }
    pitchee_string_free(json);
    pitchee_analyzer_destroy(analyzer);
    return 0;
}

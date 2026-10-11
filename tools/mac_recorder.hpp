#ifndef PITCHEE_MAC_RECORDER_HPP
#define PITCHEE_MAC_RECORDER_HPP

#include <string>

bool record_macos_audio(
    const std::string& output_path,
    double duration_seconds,
    std::string& error
);

#endif

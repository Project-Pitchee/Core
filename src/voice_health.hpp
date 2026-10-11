#ifndef PITCHEE_VOICE_HEALTH_HPP
#define PITCHEE_VOICE_HEALTH_HPP

#include "internal.hpp"

#include <vector>

namespace pitchee {

std::vector<SampleWindow> voice_health_windows(size_t sample_count);

VoiceHealthResult analyze_voice_health(
    const std::vector<float>& signal,
    const std::vector<std::vector<float>>& embeddings,
    const PitchResult& pitch,
    const VadResult& vad
);

}  // namespace pitchee

#endif

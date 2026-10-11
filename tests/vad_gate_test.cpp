// The voice-health VAD gate: profile separation and the assessability floor.
//
// Runs on the lightweight energy backend (PITCHEE_VAD_BACKEND=light) so no
// model file is needed. What matters here is the post-processing shared by
// both backends: the periodicity gate, the voiced-ratio gate, the minimum
// speech floor, and the rejection reason each of them reports.

#include "vad.hpp"
#include "internal.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

// A periodic buzz at `f0`, loud enough to trigger the energy backend.
std::vector<float> voiced(double seconds, double f0 = 120.0, float gain = 0.3f) {
    const size_t count = static_cast<size_t>(seconds * pitchee::kSampleRate);
    std::vector<float> samples(count);
    for (size_t index = 0; index < count; ++index) {
        const double phase = 2.0 * 3.14159265358979323846 * f0
            * static_cast<double>(index) / pitchee::kSampleRate;
        samples[index] = gain * static_cast<float>(
            std::sin(phase) + 0.5 * std::sin(2.0 * phase));
    }
    return samples;
}

std::vector<float> silence(double seconds) {
    return std::vector<float>(
        static_cast<size_t>(seconds * pitchee::kSampleRate), 0.0f);
}

std::vector<float> join(const std::vector<std::vector<float>>& parts) {
    std::vector<float> all;
    for (const auto& part : parts) all.insert(all.end(), part.begin(), part.end());
    return all;
}

}  // namespace

int main() {
    setenv("PITCHEE_VAD_BACKEND", "light", 1);
    const pitchee::VadDetector detector("unused-model-path", 1);

    // The general profile is the product-wide operating point and the two
    // profiles must stay distinct: the health profile only ever loosens.
    const pitchee::VadThresholds general = pitchee::general_vad_thresholds();
    const pitchee::VadThresholds health = pitchee::voice_health_vad_thresholds();
    require(general.speech_start_probability == 0.60, "general start threshold");
    require(general.speech_end_probability == 0.30, "general end threshold");
    require(general.minimum_periodicity == 0.50, "general periodicity");
    require(general.minimum_voiced_ratio == 0.12, "general voiced ratio");
    require(general.minimum_speech_seconds == 0.0, "general has no speech floor");
    require(health.speech_start_probability == general.speech_start_probability,
            "health profile must not loosen the Silero start gate");
    require(health.speech_end_probability == general.speech_end_probability,
            "health profile must not loosen the Silero end gate");
    require(health.minimum_periodicity < general.minimum_periodicity,
            "health profile loosens periodicity");
    require(health.minimum_voiced_ratio < general.minimum_voiced_ratio,
            "health profile loosens voiced ratio");
    require(health.minimum_speech_seconds > 0.0,
            "health profile refuses clips too short to measure");

    // The energy backend keys off contrast with the noise floor, so the
    // fixtures put the voicing between silent stretches.
    const std::vector<float> sustained =
        join({silence(2.0), voiced(6.0), silence(2.0)});
    const pitchee::VadResult implicit = detector.detect(sustained);
    require(implicit.thresholds.minimum_periodicity == general.minimum_periodicity,
            "detect() without thresholds uses the general profile");
    require(!implicit.segments.empty(), "sustained voicing is speech");
    require(implicit.rejection == pitchee::VadRejection::kNone,
            "accepted clip reports no rejection");

    // Silence is rejected before the periodicity gate can see it, under both
    // profiles: loosening the health gate must not admit non-speech.
    for (const pitchee::VadThresholds& profile : {general, health}) {
        const pitchee::VadResult quiet = detector.detect(silence(4.0), profile);
        require(quiet.segments.empty(), "silence is not speech");
        require(quiet.rejection == pitchee::VadRejection::kSileroProbability,
                "silence is rejected at the first gate");
        require(quiet.speech_seconds == 0.0, "silence has no speech seconds");
    }

    // The speech floor: a clip with real but very short voicing is reported as
    // insufficient rather than scored from near-empty feature frames.
    const std::vector<float> blip = join({silence(1.0), voiced(0.2), silence(1.0)});
    pitchee::VadThresholds strict_floor = health;
    strict_floor.minimum_speech_seconds = 2.0;
    const pitchee::VadResult short_clip = detector.detect(blip, strict_floor);
    require(short_clip.segments.empty(), "0.2 s of voicing is not assessable");
    require(short_clip.rejection == pitchee::VadRejection::kInsufficientSpeech,
            "short clip rejection names the speech floor");
    require(short_clip.speech_seconds == 0.0,
            "rejected clip reports no usable speech");

    // Same clip, no floor: it is accepted, which is why the floor exists.
    pitchee::VadThresholds no_floor = health;
    no_floor.minimum_speech_seconds = 0.0;
    const pitchee::VadResult unfloored = detector.detect(blip, no_floor);
    require(!unfloored.segments.empty(), "without a floor the blip is accepted");
    require(unfloored.speech_seconds > 0.0, "accepted blip reports speech");
    require(unfloored.speech_seconds < strict_floor.minimum_speech_seconds,
            "accepted blip is below the health floor");

    // A long clip clears the floor under the health profile.
    const pitchee::VadResult long_clip = detector.detect(sustained, health);
    require(!long_clip.segments.empty(), "6 s of voicing is assessable");
    require(long_clip.speech_seconds >= health.minimum_speech_seconds,
            "accepted clip clears its own floor");
    require(long_clip.thresholds.minimum_speech_seconds
                == health.minimum_speech_seconds,
            "result carries the operating point it was produced with");

    std::cout << "vad gate tests passed\n";
    return 0;
}

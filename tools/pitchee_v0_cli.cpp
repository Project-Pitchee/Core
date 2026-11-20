#include "pitchee/v0.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

const char* class_name(int predicted_class) {
    switch (predicted_class) {
        case PITCHEE_V0_CLASS_MALE_NATURAL: return "male_natural";
        case PITCHEE_V0_CLASS_MALE_UNNATURAL: return "male_unnatural";
        case PITCHEE_V0_CLASS_FEMALE_NATURAL: return "female_natural";
        default: return "unknown";
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* model_path = "models";
    const char* audio_path = nullptr;
    bool json = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--model") == 0 && index + 1 < argc) {
            model_path = argv[++index];
        } else if (std::strcmp(argv[index], "--json") == 0) {
            json = true;
        } else if (argv[index][0] != '-') {
            audio_path = argv[index];
        }
    }
    if (!audio_path) {
        std::fprintf(stderr, "usage: pitchee_v0_cli [--json] [--model models] audio.wav\n");
        return 2;
    }

    char error[1024] = {};
    pitchee_v0_t* core = nullptr;
    pitchee_status_t status = pitchee_v0_create(
        model_path,
        2,
        0,
        &core,
        error,
        sizeof(error)
    );
    if (status != PITCHEE_SUCCESS) {
        std::fprintf(stderr, "create failed: %s\n", error);
        return static_cast<int>(status);
    }

    pitchee_v0_result_t result{};
    result.struct_size = sizeof(result);
    result.version = PITCHEE_V0_RESULT_VERSION;
    status = pitchee_v0_analyze_wav_file(
        core,
        audio_path,
        &result,
        error,
        sizeof(error)
    );
    pitchee_v0_destroy(core);
    if (status != PITCHEE_SUCCESS) {
        std::fprintf(stderr, "analysis failed: %s\n", error);
        return static_cast<int>(status);
    }

    if (json) {
        std::printf(
            "{\n"
            "  \"model\": \"%s\",\n"
            "  \"prediction\": \"%s\",\n"
            "  \"confidence\": %.9f,\n"
            "  \"probabilities\": {\n"
            "    \"male_natural\": %.9f,\n"
            "    \"male_unnatural\": %.9f,\n"
            "    \"female_natural\": %.9f\n"
            "  },\n"
            "  \"timbre_score\": %.6f,\n"
            "  \"timbre_base\": %.6f,\n"
            "  \"timbre_cap\": %.6f,\n"
            "  \"naturalness_score\": %.6f,\n"
            "  \"passed\": %s,\n"
            "  \"source_seconds\": %.6f,\n"
            "  \"analyzed_seconds\": %.6f,\n"
            "  \"speech_seconds\": %.6f,\n"
            "  \"preprocess_ms\": %.3f,\n"
            "  \"inference_ms\": %.3f,\n"
            "  \"postprocess_ms\": %.3f\n"
            "}\n",
            pitchee_v0_version(),
            class_name(result.predicted_class),
            result.confidence,
            result.probabilities[0],
            result.probabilities[1],
            result.probabilities[2],
            result.timbre_score,
            result.timbre_base,
            result.timbre_cap,
            result.naturalness_score,
            result.passed ? "true" : "false",
            result.source_seconds,
            result.analyzed_seconds,
            result.speech_seconds,
            result.preprocess_ms,
            result.inference_ms,
            result.postprocess_ms
        );
    } else {
        std::printf("prediction: %s\n", class_name(result.predicted_class));
        std::printf(
            "probabilities: male_natural %.6f male_unnatural %.6f female_natural %.6f\n",
            result.probabilities[0],
            result.probabilities[1],
            result.probabilities[2]
        );
        std::printf("confidence: %.6f\n", result.confidence);
        std::printf("timbre: %.2f (base %.2f cap %.2f)\n", result.timbre_score,
                    result.timbre_base, result.timbre_cap);
        std::printf("naturalness: %.2f\n", result.naturalness_score);
        std::printf("pass: %s\n", result.passed ? "yes" : "no");
        std::printf("audio: %.3fs source, %.3fs analyzed, %.3fs speech\n",
                    result.source_seconds, result.analyzed_seconds,
                    result.speech_seconds);
    }
    return 0;
}

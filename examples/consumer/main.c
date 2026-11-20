#include "pitchee/v0.h"

#include <stdio.h>

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: pitchee_consumer models voice.wav\n");
        return 2;
    }

    char error[1024] = {0};
    pitchee_v0_t* core = NULL;
    pitchee_status_t status = pitchee_v0_create(
        argv[1], 2, 0, &core, error, sizeof(error)
    );
    if (status != PITCHEE_SUCCESS) {
        fprintf(stderr, "create failed: %s\n", error);
        return (int)status;
    }

    pitchee_v0_result_t result = {
        .struct_size = sizeof(result),
        .version = PITCHEE_V0_RESULT_VERSION,
    };
    status = pitchee_v0_analyze_wav_file(
        core, argv[2], &result, error, sizeof(error)
    );
    pitchee_v0_destroy(core);
    if (status != PITCHEE_SUCCESS) {
        fprintf(stderr, "analysis failed: %s\n", error);
        return (int)status;
    }

    printf(
        "class=%d confidence=%.4f timbre=%.2f naturalness=%.2f pass=%d\n",
        result.predicted_class,
        result.confidence,
        result.timbre_score,
        result.naturalness_score,
        result.passed
    );
    return 0;
}

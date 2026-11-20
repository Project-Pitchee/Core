#include "pitchee/v0.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    if (strcmp(pitchee_v0_version(), "v0-wavlm-large-l12-hqq4-b128") != 0) {
        fprintf(stderr, "unexpected V0 version\n");
        return 1;
    }
    if (strcmp(
            pitchee_v0_expected_model_sha256(),
            "54f1499a09d88e585c0bebe02e049be46d4bc3bcc15afa42d6aec981ecbb81a9"
        ) != 0) {
        fprintf(stderr, "unexpected V0 model hash\n");
        return 1;
    }
    if (fabs(pitchee_v0_timbre_score(0.5, 3.0) - 39.0) > 0.001
        || fabs(pitchee_v0_naturalness_score(0.5, 3.0) - 50.0) > 0.001) {
        fprintf(stderr, "unexpected V0 score mapping\n");
        return 1;
    }
    if (pitchee_v0_create(NULL, 2, 0, NULL, NULL, 0)
        != PITCHEE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "unexpected V0 create status\n");
        return 1;
    }
    if (pitchee_v0_analyze_pcm(
            NULL,
            NULL,
            0,
            16000,
            1,
            NULL,
            NULL,
            0
        ) != PITCHEE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "unexpected V0 PCM status\n");
        return 1;
    }
    return 0;
}

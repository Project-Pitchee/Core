#include "pitchee/pitchee.h"
#include "pitchee/spectrum.h"
#include "pitchee/hnr.h"

#include <stdio.h>
#include <string.h>

static void progress_callback(const pitchee_progress_t* progress, void* user_data) {
    (void)progress;
    (void)user_data;
}

int main(void) {
    (void)&pitchee_analysis_run_voice_health;
    (void)&pitchee_analysis_run_voice_health_wav_file;

    if (strcmp(pitchee_version(), "0.5.0") != 0) {
        fprintf(stderr, "unexpected version\n");
        return 1;
    }

    pitchee_score_result_t score;
    if (pitchee_score(
            PITCHEE_SCORE_PROFILE_FEMINIZATION,
            63.0,
            90.0,
            0.0,
            &score
        )
        != PITCHEE_SUCCESS) {
        fprintf(stderr, "score call failed\n");
        return 1;
    }
    if (score.final_score < 62.999 || score.final_score > 63.001) {
        fprintf(stderr, "unexpected fallback score\n");
        return 1;
    }
    if (strcmp(score.score_rule, "f0_unavailable") != 0) {
        fprintf(stderr, "unexpected score rule\n");
        return 1;
    }

    if (pitchee_analyze_wav_file(
            NULL,
            "missing.wav",
            PITCHEE_SCORE_PROFILE_FEMINIZATION,
            progress_callback,
            NULL,
            NULL,
            NULL,
            0
        ) != PITCHEE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "unexpected progress API status\n");
        return 1;
    }

    if (pitchee_analyzer_detect_vad(
            NULL,
            NULL,
            0,
            16000,
            1,
            NULL,
            NULL,
            NULL,
            0
        ) != PITCHEE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "unexpected VAD API status\n");
        return 1;
    }

    if (pitchee_realtime_f0_create(
            NULL,
            5120,
            256,
            NULL,
            NULL,
            0
        ) != PITCHEE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "unexpected realtime F0 create status\n");
        return 1;
    }
    if (pitchee_realtime_f0_process(
            NULL,
            NULL,
            0,
            NULL,
            NULL,
            NULL,
            0
        ) != PITCHEE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "unexpected realtime F0 process status\n");
        return 1;
    }
    {
        const float silence[64] = {0};
        pitchee_hnr_summary_t summary;
        if (pitchee_hnr_analyze(silence, 64, NULL, 0, NULL, NULL, &summary) != PITCHEE_SUCCESS
            || summary.has_hnr != 0 || summary.hnr_window_count != 0
            || summary.window_seconds != 0.04) {
            fprintf(stderr, "unexpected HNR status\n");
            return 1;
        }
    }
    if (pitchee_spectrum_create(
            NULL,
            NULL,
            NULL,
            0
        ) != PITCHEE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "unexpected spectrum create status\n");
        return 1;
    }
    if (pitchee_spectrum_process(
            NULL,
            NULL,
            0,
            NULL,
            NULL,
            NULL,
            0
        ) != PITCHEE_ERROR_INVALID_ARGUMENT) {
        fprintf(stderr, "unexpected spectrum process status\n");
        return 1;
    }
    return 0;
}

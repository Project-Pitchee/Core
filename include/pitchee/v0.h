#ifndef PITCHEE_V0_H
#define PITCHEE_V0_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PITCHEE_V0_RESULT_VERSION 1u
#define PITCHEE_V0_DEFAULT_MODEL "wavlm-large-l12-hqq4-b128-convw8.onnx"
#define PITCHEE_V0_SAMPLE_RATE 16000
#define PITCHEE_V0_WINDOW_SECONDS 12
#define PITCHEE_V0_CLASS_COUNT 3

typedef struct pitchee_v0_t pitchee_v0_t;

typedef enum pitchee_v0_class_t {
    PITCHEE_V0_CLASS_MALE_NATURAL = 0,
    PITCHEE_V0_CLASS_MALE_UNNATURAL = 1,
    PITCHEE_V0_CLASS_FEMALE_NATURAL = 2
} pitchee_v0_class_t;

typedef struct pitchee_v0_result_t {
    uint32_t struct_size;
    uint32_t version;
    int32_t predicted_class;
    int32_t passed;
    double probabilities[3];
    double confidence;
    double timbre_score;
    double timbre_base;
    double timbre_cap;
    double naturalness_score;
    double source_seconds;
    double analyzed_seconds;
    double speech_seconds;
    double preprocess_ms;
    double inference_ms;
    double postprocess_ms;
} pitchee_v0_result_t;

PITCHEE_API const char* pitchee_v0_version(void);

/* Expected SHA-256 of PITCHEE_V0_DEFAULT_MODEL, as lowercase hex. */
PITCHEE_API const char* pitchee_v0_expected_model_sha256(void);

/*
 * model_directory may contain PITCHEE_V0_DEFAULT_MODEL, or be a path to an
 * ONNX model directly. The model SHA-256 is verified before the runtime is
 * created. Zero intra_op_threads selects two threads.
 */
PITCHEE_API pitchee_status_t pitchee_v0_create(
    const char* model_directory,
    int32_t intra_op_threads,
    int32_t use_coreml,
    pitchee_v0_t** out_core,
    char* error_message,
    size_t error_message_capacity
);

PITCHEE_API void pitchee_v0_destroy(pitchee_v0_t* core);

/*
 * Samples are mono or interleaved float32 PCM in approximately [-1, 1].
 * The most speech-dense window of at most 12 seconds is analyzed. A result
 * with version 1 and struct_size == sizeof(pitchee_v0_result_t) is required.
 */
PITCHEE_API pitchee_status_t pitchee_v0_analyze_pcm(
    pitchee_v0_t* core,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    pitchee_v0_result_t* out_result,
    char* error_message,
    size_t error_message_capacity
);

PITCHEE_API pitchee_status_t pitchee_v0_analyze_wav_file(
    pitchee_v0_t* core,
    const char* wav_path,
    pitchee_v0_result_t* out_result,
    char* error_message,
    size_t error_message_capacity
);

/* Returns a display timbre score in [1, 99] without the confidence ceiling. */
PITCHEE_API double pitchee_v0_timbre_score(double female_probability, double softness_k);

/* Returns a display naturalness score in [1, 99]. */
PITCHEE_API double pitchee_v0_naturalness_score(double natural_probability, double softness_k);

#ifdef __cplusplus
}
#endif

#endif

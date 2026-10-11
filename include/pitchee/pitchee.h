#ifndef PITCHEE_PITCHEE_H
#define PITCHEE_PITCHEE_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct pitchee_analyzer_t pitchee_analyzer_t;
typedef struct pitchee_realtime_f0_t pitchee_realtime_f0_t;



/* Required by every analysis and score call. */
typedef enum pitchee_score_profile_t {
    PITCHEE_SCORE_PROFILE_FEMINIZATION = 0,
    PITCHEE_SCORE_PROFILE_MASCULINIZATION = 1
} pitchee_score_profile_t;

typedef enum pitchee_progress_stage_t {
    PITCHEE_PROGRESS_STAGE_LOADING_AUDIO = 0,
    PITCHEE_PROGRESS_STAGE_RESAMPLING_AUDIO = 1,
    PITCHEE_PROGRESS_STAGE_ANALYZING_F0 = 2,
    PITCHEE_PROGRESS_STAGE_DETECTING_SPEECH = 3,
    PITCHEE_PROGRESS_STAGE_PREPARING_VFP_WINDOWS = 4,
    PITCHEE_PROGRESS_STAGE_EXTRACTING_VFP_EMBEDDINGS = 5,
    PITCHEE_PROGRESS_STAGE_CLASSIFYING_VFP_WINDOWS = 6,
    PITCHEE_PROGRESS_STAGE_PREPARING_NATURALNESS_WINDOWS = 7,
    PITCHEE_PROGRESS_STAGE_EXTRACTING_NATURALNESS_EMBEDDINGS = 8,
    PITCHEE_PROGRESS_STAGE_SCORING_NATURALNESS_WINDOWS = 9,
    PITCHEE_PROGRESS_STAGE_CALCULATING_SCORES = 10,
    PITCHEE_PROGRESS_STAGE_SERIALIZING_RESULT = 11,
    PITCHEE_PROGRESS_STAGE_COMPLETED = 12
} pitchee_progress_stage_t;

typedef struct pitchee_progress_t {
    pitchee_progress_stage_t stage;
    uint64_t completed;
    uint64_t total;
    double fraction;
} pitchee_progress_t;

typedef void (*pitchee_progress_callback_t)(
    const pitchee_progress_t* progress,
    void* user_data
);

typedef struct pitchee_f0_frame_t {
    double timestamp_seconds;
    float f0_hz;
    float confidence;
    int32_t voiced;
} pitchee_f0_frame_t;

typedef void (*pitchee_f0_frame_callback_t)(
    const pitchee_f0_frame_t* frame,
    void* user_data
);

#define PITCHEE_REALTIME_F0_OPTIONS_VERSION 1u
#define PITCHEE_REALTIME_F0_METADATA_VERSION 1u
#define PITCHEE_REALTIME_F0_STATS_VERSION 1u

/*
 * The stream owns a fixed context shape. To change context_samples or
 * inference_hop_samples, create a new stream.
 *
 * output_rate_hz <= 0 selects the native SwiftF0 frame rate. A requested rate
 * above the native frame rate is capped to it.
 */
typedef struct pitchee_realtime_f0_options_t {
    uint32_t struct_size;
    uint32_t version;
    int32_t context_samples;
    int32_t inference_hop_samples;
    double output_rate_hz;
    double start_timestamp_seconds;
} pitchee_realtime_f0_options_t;

typedef struct pitchee_realtime_f0_metadata_t {
    uint32_t struct_size;
    uint32_t version;
    int32_t sample_rate;
    int32_t frame_hop_samples;
    double frame_interval_seconds;
    int32_t min_context_samples;
    double model_output_rate_hz;
    double effective_output_rate_hz;
} pitchee_realtime_f0_metadata_t;

typedef struct pitchee_realtime_f0_stats_t {
    uint32_t struct_size;
    uint32_t version;
    uint64_t process_calls;
    uint64_t inference_calls;
    uint64_t emitted_frames;
    double total_ms;
    double last_total_ms;
    double average_total_ms;
    double last_preprocess_ms;
    double average_preprocess_ms;
    double last_model_ms;
    double average_model_ms;
    double last_postprocess_ms;
    double average_postprocess_ms;
    double measured_output_hz;
} pitchee_realtime_f0_stats_t;

typedef struct pitchee_score_result_t {
    double base_score;
    double final_score;
    double score_cap;
    int32_t has_score_cap;
    int32_t score_limited;
    int32_t score_boosted;
    char score_rule[32];
} pitchee_score_result_t;

PITCHEE_API const char* pitchee_version(void);

/*
 * Zero values for intra_op_threads and min_confidence select 2 and 0.9.
 * Zero values for min_f0_hz and max_f0_hz select 75 Hz and 600 Hz.
 */
PITCHEE_API pitchee_status_t pitchee_analyzer_create(
    const char* model_directory,
    int32_t intra_op_threads,
    int32_t use_coreml,
    float min_f0_hz,
    float max_f0_hz,
    float min_confidence,
    pitchee_analyzer_t** out_analyzer,
    char* error_message,
    size_t error_message_capacity
);

PITCHEE_API void pitchee_analyzer_destroy(pitchee_analyzer_t* analyzer);

/*
 * Detect VAD intervals without running pitch, VFP, naturalness, or HNR.
 * Input may be any mono/stereo float32 PCM; Core resamples it to 16 kHz.
 * Segments are reported on the original input timeline. Callbacks run
 * synchronously on the calling thread and the segment pointer is borrowed only
 * for that callback.
 */
PITCHEE_API pitchee_status_t pitchee_analyzer_detect_vad(
    pitchee_analyzer_t* analyzer,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    pitchee_vad_segment_callback_t segment_callback,
    void* user_data,
    char* error_message,
    size_t error_message_capacity
);

/*
 * Samples must be mono or interleaved float32 PCM in [-1, 1].
 * The implementation resamples to 16 kHz, so any positive source rate is valid.
 * The returned JSON string is allocated by PitcheeCore and must be released with
 * pitchee_string_free().
 */
PITCHEE_API pitchee_status_t pitchee_analyze_pcm(
    pitchee_analyzer_t* analyzer,
    const float* samples,
    size_t sample_count,
    int32_t sample_rate,
    int32_t channels,
    pitchee_score_profile_t score_profile,
    pitchee_progress_callback_t progress_callback,
    void* user_data,
    char** out_json,
    char* error_message,
    size_t error_message_capacity
);

PITCHEE_API pitchee_status_t pitchee_analyze_wav_file(
    pitchee_analyzer_t* analyzer,
    const char* wav_path,
    pitchee_score_profile_t score_profile,
    pitchee_progress_callback_t progress_callback,
    void* user_data,
    char** out_json,
    char* error_message,
    size_t error_message_capacity
);

PITCHEE_API pitchee_status_t pitchee_realtime_f0_create(
    pitchee_analyzer_t* analyzer,
    int32_t context_samples,
    int32_t hop_samples,
    pitchee_realtime_f0_t** out_stream,
    char* error_message,
    size_t error_message_capacity
);

/*
 * Synchronous: frame_callback is invoked on the calling thread, only before
 * this function returns. Neither frame_callback nor user_data is retained.
 * Each frame pointer is valid only for that callback invocation; copy values
 * needed afterward. The samples pointer is also borrowed only for this call.
 */
PITCHEE_API pitchee_status_t pitchee_realtime_f0_create_with_options(
    pitchee_analyzer_t* analyzer,
    const pitchee_realtime_f0_options_t* options,
    pitchee_realtime_f0_t** out_stream,
    char* error_message,
    size_t error_message_capacity
);
PITCHEE_API pitchee_status_t pitchee_realtime_f0_process(
    pitchee_realtime_f0_t* stream,
    const float* samples,
    size_t sample_count,
    pitchee_f0_frame_callback_t frame_callback,
    void* user_data,
    char* error_message,
    size_t error_message_capacity
);

PITCHEE_API void pitchee_realtime_f0_reset(pitchee_realtime_f0_t* stream);

PITCHEE_API void pitchee_realtime_f0_destroy(pitchee_realtime_f0_t* stream);

PITCHEE_API pitchee_status_t pitchee_realtime_f0_get_metadata(
    const pitchee_realtime_f0_t* stream,
    pitchee_realtime_f0_metadata_t* out_metadata,
    char* error_message,
    size_t error_message_capacity
);

PITCHEE_API pitchee_status_t pitchee_realtime_f0_warmup(
    pitchee_realtime_f0_t* stream,
    char* error_message,
    size_t error_message_capacity
);

PITCHEE_API pitchee_status_t pitchee_realtime_f0_get_stats(
    const pitchee_realtime_f0_t* stream,
    pitchee_realtime_f0_stats_t* out_stats,
    char* error_message,
    size_t error_message_capacity
);

/*
 * Pass NAN or a non-positive f0_hz when F0 is unavailable.
 * The final score is clamped to 0-100.
 */
PITCHEE_API pitchee_status_t pitchee_score(
    pitchee_score_profile_t score_profile,
    double vfp_standard_score,
    double naturalness_score,
    double f0_hz,
    pitchee_score_result_t* out_score
);

PITCHEE_API void pitchee_string_free(char* value);

#ifdef __cplusplus
}
#endif

#endif

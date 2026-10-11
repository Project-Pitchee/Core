#ifndef PITCHEE_HNR_H
#define PITCHEE_HNR_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Existing VAD intervals on the original 16 kHz PCM timeline, in seconds. */
typedef pitchee_vad_segment_t pitchee_hnr_vad_segment_t;

typedef struct pitchee_hnr_window_t {
    double start_seconds;
    double end_seconds;
    double hnr_db;
    int32_t has_hnr;
    int32_t reserved;
} pitchee_hnr_window_t;

typedef struct pitchee_hnr_summary_t {
    double hnr_db;
    double hnr_std_db;
    double window_seconds;
    int32_t hnr_window_count;
    int32_t has_hnr;
} pitchee_hnr_summary_t;

typedef void (*pitchee_hnr_window_callback_t)(
    const pitchee_hnr_window_t* window,
    void* user_data
);

/*
 * The caller MUST supply 16 kHz mono Float32 PCM; no sample-rate/channel
 * conversion is performed, and all lag and window sizes assume this format.
 * HNR consumes 16 kHz mono Float32 PCM and precomputed VAD intervals; it does
 * not run a VAD or load models. Windows are 40 ms with a 10 ms hop. A window's
 * actual (clipped) midpoint must be in a VAD interval [start, end) before any
 * PCM is examined. An empty interval list therefore yields only nil windows.
 * Intervals must be finite, nonnegative, sorted, nonoverlapping, with start <
 * end. Intervals may extend past the input duration; only input windows are
 * queried. Adjacent intervals are allowed. NULL is valid only for count == 0.
 * No heap allocation or locks. out_summary is required; callback may be NULL.
 * Empty PCM succeeds with no windows and has_hnr == 0.
 * All callbacks run synchronously on the calling thread before return.
 * No PCM/VAD/callback/user_data pointer is retained. A window pointer is valid
 * only during its callback; copy any values needed afterward.
 */
PITCHEE_API pitchee_status_t pitchee_hnr_analyze(
    const float* samples,
    size_t sample_count,
    const pitchee_hnr_vad_segment_t* vad_segments,
    size_t vad_segment_count,
    pitchee_hnr_window_callback_t window_callback,
    void* user_data,
    pitchee_hnr_summary_t* out_summary
);

#ifdef __cplusplus
}
#endif

#endif

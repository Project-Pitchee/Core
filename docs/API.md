# C ABI

PitcheeCore `0.5.0` keeps the main ABI focused on four operations:

1. Create one reusable analyzer.
2. Analyze an audio buffer or WAV file.
3. Run a realtime F0 stream.
4. Calculate a composite score from three metrics.

Spectrum has its own header and is independent of model loading.

```c
#include <pitchee/pitchee.h>
#include <pitchee/spectrum.h>
```

## Analyzer lifetime

Analyzer creation owns all common runtime settings and all ONNX Runtime sessions:

```c
pitchee_analyzer_t* analyzer = NULL;
char error[1024] = {0};

pitchee_status_t status = pitchee_analyzer_create(
    model_directory,
    2,       /* intra_op_threads */
    0,       /* use_coreml */
    75.0f,   /* min_f0_hz */
    600.0f,  /* max_f0_hz */
    0.9f,    /* min SwiftF0 confidence */
    &analyzer,
    error,
    sizeof(error)
);
```

Parameter rules:

- `intra_op_threads <= 0` selects `2`.
- `use_coreml == 0` disables CoreML; non-zero enables it.
- `min_f0_hz <= 0` selects `75`.
- `max_f0_hz <= 0` selects `600`.
- `min_confidence <= 0` selects `0.9`.
- Realtime F0 streams inherit these F0 thresholds.

The analyzer is reusable across audio buffers. Calls on one analyzer must be
serialized, or each worker should create its own analyzer.

Destroy it with `pitchee_analyzer_destroy(analyzer)`.

## Score profile

Every offline analysis and score call requires:

```c
PITCHEE_SCORE_PROFILE_FEMINIZATION
PITCHEE_SCORE_PROFILE_MASCULINIZATION
```

The profile selects the composite scoring rule. It does not change the raw F0,
VFP, or Naturalness model outputs.

## Analyze PCM

```c
char* json = NULL;
status = pitchee_analyze_pcm(
    analyzer,
    samples,
    sample_count,
    sample_rate,
    channels,
    PITCHEE_SCORE_PROFILE_FEMINIZATION,
    progress_callback,
    user_data,
    &json,
    error,
    sizeof(error)
);
```

- `samples` is Float32 PCM in `[-1, 1]`.
- `sample_count` is the total number of float values across all channels.
- `channels == 1` means mono; greater than one means interleaved.
- The library resamples to 16 kHz and does not impose a maximum duration.
- Resampled audio is quantized to PCM16 to match model training.
- `progress_callback` can be `NULL`.
- Release `json` with `pitchee_string_free(json)`.

`pitchee_progress_t` contains:

| Field | Meaning |
| --- | --- |
| `stage` | A `pitchee_progress_stage_t` constant. |
| `completed` | Completed units in the current stage. |
| `total` | Total units in the current stage. |
| `fraction` | Stage-local value in `[0, 1]`. |

Stages cover loading, resampling, F0, VAD, VFP, Naturalness, scoring,
serialization, and completion. Callbacks run synchronously.

## Analyze WAV

```c
status = pitchee_analyze_wav_file(
    analyzer,
    wav_path,
    PITCHEE_SCORE_PROFILE_FEMINIZATION,
    progress_callback,
    user_data,
    &json,
    error,
    sizeof(error)
);
```

PCM16, PCM32, and Float32 WAV files are supported. Compressed formats should be
decoded by the platform and passed to `pitchee_analyze_pcm()`.

## Realtime F0

```c
pitchee_realtime_f0_options_t options = {
    .struct_size = sizeof(options),
    .version = PITCHEE_REALTIME_F0_OPTIONS_VERSION,
    .context_samples = 5120,
    .inference_hop_samples = 256,
    .output_rate_hz = 30.0,
    .start_timestamp_seconds = 0.0,
};

pitchee_realtime_f0_t* stream = NULL;
pitchee_realtime_f0_create_with_options(
    analyzer,
    &options,
    &stream,
    error,
    sizeof(error)
);

pitchee_realtime_f0_warmup(stream, error, sizeof(error));

void on_f0_frame(const pitchee_f0_frame_t* frame, void* user_data);

pitchee_realtime_f0_process(
    stream,
    samples,
    sample_count,
    on_f0_frame,
    user_data,
    error,
    sizeof(error)
);

pitchee_realtime_f0_reset(stream);
pitchee_realtime_f0_destroy(stream);
```

Input is 16 kHz mono Float32 PCM. Each callback frame contains:

| Field | Meaning |
| --- | --- |
| `timestamp_seconds` | Selected model-frame time plus `start_timestamp_seconds`. |
| `f0_hz` | Raw SwiftF0 estimate. |
| `confidence` | SwiftF0 confidence in `[0, 1]`. |
| `voiced` | `1` when the analyzer's F0 thresholds are satisfied. |

`context_samples` is the model input window. `inference_hop_samples` controls
how often inference runs, not callback cadence. `output_rate_hz` selects a
stable callback rate from the model frames and is capped at the model rate.
Use `pitchee_realtime_f0_get_metadata()` instead of hard-coding model metadata.

The stream shape is fixed after creation. To change the context or hop, destroy
the stream and create another one. Call `pitchee_realtime_f0_warmup()` before
recording when predictable ncnn startup latency is required.

`frame_callback` runs synchronously on the thread calling
`pitchee_realtime_f0_process()`. Calls for one stream must be serialized. The
callback must not re-enter the same stream and must not call `destroy()` while a
process call is active. The analyzer must outlive every stream created from it.

`pitchee_realtime_f0_get_stats()` reports process/inference/frame counters,
preprocess/model/postprocess/total timing, and measured output rate.

## Composite score

```c
pitchee_score_result_t score;
pitchee_score(
    PITCHEE_SCORE_PROFILE_FEMINIZATION,
    vfp_standard_score,
    naturalness_score,
    f0_hz,
    &score
);
```

Pass `NAN` or a non-positive `f0_hz` when F0 is unavailable. The final value is
`score.final_score`; the other fields describe the selected rule, cap, and
whether a boost or limit was applied.

This function has no ONNX Runtime dependency.

## Voice Health

Voice Health is deliberately separate from the bundled VFP/F0/Naturalness JSON:

```c
pitchee_voice_health_result_t health = {0};
pitchee_analysis_run_voice_health_wav_file(
    analyzer,
    "voice.wav",
    NULL,
    NULL,
    &health,
    error,
    sizeof(error)
);
```

For in-memory audio, use `pitchee_analysis_run_voice_health()`. The function
reuses the analyzer's VAD, SwiftF0 and ECAPA models, then applies the embedded
Logistic head from `voice_health_model.hpp`. No extra model file is needed.
Android uses the same ncnn-backed VAD, SwiftF0 and ECAPA path.

The discriminant trained by the offline experiment was `0.4135834447`. Core
applies a monotonic piecewise-linear mapping so that the trained boundary lands
on health score `30`:

```text
raw disorder probability 0.0    -> health score 100
raw disorder probability 0.4136 -> health score  30
raw disorder probability 1.0    -> health score   0
```

The reported `disorder_probability` uses the mapped probability space and
`health_score = 100 * (1 - disorder_probability)`. `decision_threshold` is on
the 0-100 health-score scale and is `30`:

```text
health_score >= 30 -> not hoarse
health_score <  30 -> hoarse
```

## Spectrum

Spectrum is independent of analyzer creation and ONNX Runtime:

```c
#include <pitchee/spectrum.h>

pitchee_spectrum_options_t options = {
    2048, 256, 40, 8000,
    PITCHEE_SPECTRUM_DBFS, 0.65f
};

pitchee_spectrum_t* spectrum = NULL;
pitchee_spectrum_create(&options, &spectrum, error, sizeof(error));

void on_spectrum_frame(
    const pitchee_spectrum_frame_t* frame,
    void* user_data
);

pitchee_spectrum_process(
    spectrum,
    samples,
    sample_count,
    on_spectrum_frame,
    user_data,
    error,
    sizeof(error)
);

pitchee_spectrum_reset(spectrum);
pitchee_spectrum_destroy(spectrum);
```

The magnitude pointer is valid only during the callback. Input is 16 kHz mono
Float32 PCM.

## HNR

HNR is not part of `pitchee_analyze_*`. Include `pitchee/hnr.h` explicitly and
provide VAD intervals yourself:

```c
#include <pitchee/pitchee.h>
#include <pitchee/hnr.h>

/* Collect source-timeline intervals from the analyzer VAD callback. */
pitchee_analyzer_detect_vad(
    analyzer,
    pcm_16k_mono,
    sample_count,
    16000,
    1,
    collect_segment,
    &segments,
    error,
    sizeof(error)
);

pitchee_hnr_summary_t summary;
pitchee_hnr_analyze(
    pcm_16k_mono,
    sample_count,
    segments.items,
    segments.count,
    NULL,
    NULL,
    &summary
);
```

HNR accepts 16 kHz mono Float32 PCM and precomputed VAD intervals. It does not
load models or depend on analyzer. The analyzer only supplies VAD segments, so
the dependency direction remains one-way.

## Result schema

The batch result is UTF-8 JSON with these top-level sections:

```json
{
  "schema_version": 3,
  "model_version": "2026-10-f1",
  "score_profile": "feminization",
  "audio": {},
  "vad": {},
  "f0": {},
  "vfp": {},
  "naturalness": {},
  "composite": {}
}
```

`f0.windows`, `vfp.windows`, and `naturalness.windows` use the original analyzed
audio timeline. The result contains data only: no UI geometry, labels, player
state, or color information.

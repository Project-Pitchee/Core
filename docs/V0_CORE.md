# V0 Core

V0 is a standalone three-class voice model. It contains no legacy analyzer,
VAD, pitch, health, or auxiliary diagnostic pipeline and returns:

- `male_natural`
- `male_unnatural`
- `female_natural` (trans women and cis women share this class)

V0 does not emit F0, HNR, jitter, or diagnostic axes. Stylized voices such as a
high, bright, or otherwise deliberate feminine delivery are valid examples of
`female_natural`; "stylized" is not treated as "unnatural".

## Model

| Item | Value |
| --- | --- |
| Backbone | WavLM Large, layer 12 mean pooling |
| Training quantization | HQQ 4-bit, group 128, convolution width 8 |
| ONNX model | `models/wavlm-large-l12-hqq4-b128-convw8.onnx`, 95 MB |
| ONNX SHA-256 | `54f1499a09d88e585c0bebe02e049be46d4bc3bcc15afa42d6aec981ecbb81a9` |
| Head | Logistic regression compiled into Core |
| Head SHA-256 | `8688dabfe25fd2fb93fbcd9e0c65507ef9b3be0ca58a1cfef2f6c3da8c1109fd` |

The classifier head is generated from the training bundle with
`tools/gen/export_v0_head.py`. The runtime does not load Python, scikit-learn,
or joblib. The ONNX file is the single deployment representation on every
platform.

## Audio contract

- Input is mono or interleaved Float32 PCM, or a PCM16/PCM32/Float32 WAV.
- Native model input is 16 kHz (`PITCHEE_V0_SAMPLE_RATE`). Other rates are
  resampled with the same polyphase filter used by the Python reference.
- Audio is mixed to mono, resampled to 16 kHz, and mean-subtracted.
- One analysis window is used. It is never an aggregate over multiple windows.
- The analyzed window is at most 12 seconds (`PITCHEE_V0_WINDOW_SECONDS`).
- Audio shorter than 12 seconds is not padded to 12 seconds. It is only padded
  to the 0.4 second minimum input.
- For long audio, Core reproduces the V0 training/evaluation window selection,
  including its 150 ms lead-in margin and the 12 second hop-aligned crop.
- A degenerate all-silent long input is still capped at 12 seconds.

`analyzed_seconds` reports the actual model input duration. `source_seconds`
reports the duration after decoding and resampling, and `speech_seconds`
reports active RMS-frame duration across the whole input.

The CTest target `pitchee_v0_integration_test` runs the real model through the C
ABI. It verifies that a 20 second input is reduced to exactly 12.0 seconds and
that a 0.1 second input is padded only to the 0.4 second minimum.

## Output and scoring

Probabilities are temperature-scaled with `T = 1.2381970150963135`.

```text
confidence    = max(P(male_natural), P(male_unnatural), P(female_natural))
timbre_base   = piecewise calibration of P(female_natural)
timbre_cap    = 49.9                                  if confidence < 0.85
              = 50 + 50 * (confidence - 0.85) / 0.15 otherwise
timbre        = min(timbre_base, timbre_cap)
passed        = timbre >= 50
naturalness   = sigmoid_calibration(P(male_natural) + P(female_natural))
```

The piecewise anchors, K=3 softening, and 1-99 display bounds are in
`src/v0/v0.cpp` and match `vf3/scoring.py`.

## C API

```c
#include <pitchee/v0.h>

pitchee_v0_t* core = NULL;
char error[1024] = {0};
pitchee_status_t status = pitchee_v0_create(
    "models",
    2,  /* intra-op threads */
    0,  /* CoreML */
    &core,
    error,
    sizeof(error)
);

pitchee_v0_result_t result = {
    .struct_size = sizeof(result),
    .version = PITCHEE_V0_RESULT_VERSION,
};
status = pitchee_v0_analyze_wav_file(
    core,
    "voice.wav",
    &result,
    error,
    sizeof(error)
);
pitchee_v0_destroy(core);
```

Use `pitchee_v0_analyze_pcm()` when the platform decoder already provides
Float32 PCM. Decode M4A/AAC/MP3/Opus outside Core; Core only accepts WAV files.

One `pitchee_v0_t` owns one ONNX Runtime session. Calls on the same instance
must be serialized, or each worker should create its own instance.

## CLI

```bash
./build-v0/pitchee_v0_cli --json --model models voice.wav
```

The JSON output contains the class, all three probabilities, confidence,
timbre, naturalness, pass state, source/analyzed/speech duration, and stage
timings.

## Reference parity

`tools/verify_v0_parity.py` reimplements the Python audio, ONNX, classifier, and
scoring path and compares it with `pitchee_v0_cli` at 16 kHz, 48 kHz, and
44.1 kHz. It also handles compressed inputs by decoding them to the same 16 kHz
PCM16 WAV used by the training pipeline.

```bash
python tools/verify_v0_parity.py \
  /path/to/reference.wav \
  /path/to/reference.m4a
```

Current acceptance target on the original 60 second target clip:

```text
prediction: female_natural
P(female_natural): 0.788419
analyzed_seconds: 12.0
```

The C++ runtime is checked against the Python reference by
`tools/verify_v0_parity.py`. The ONNX model hash is verified before a session
is created, and the integration test rejects a corrupted model file.

## Platform status

- Desktop macOS/Linux/Windows: ONNX Runtime.
- iOS: ONNX Runtime linked by the host application.
- Android: ONNX Runtime Android AAR with the same ONNX asset as desktop.

## Mobile cost

- V0 ONNX model: `99,112,953` bytes (94.5 MiB).
- Desktop Apple M2, 12 second input, CPU: about 2.26 seconds per inference.
- Mobile latency depends on the device and ONNX Runtime execution provider;
  measure it on the target phone rather than reusing desktop numbers.

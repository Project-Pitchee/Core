# Platform integration

## What the shared core owns

- FFmpeg-compatible PCM resampling to 16 kHz
- Silero VAD and breath filtering
- ECAPA log-Mel frontend
- ECAPA embeddings
- VFP probabilities
- Naturalness aggregate and per-window scores
- 0.05-second SwiftF0 pitch timeline
- Realtime chunk-buffered SwiftF0 frames
- Streaming STFT spectrum frames and spectral summary metrics
- HNR autocorrelation with independent 40 ms windows / 10 ms hops,
  gated by caller-supplied existing VAD intervals on the original PCM timeline
- Composite score

## What each platform owns

### iOS

- AVAudioEngine or AVAudioRecorder for capture
- AVAudioFile or AVAssetReader for file decoding
- AVAudioPlayer for playback
- SwiftUI or another UI framework for presentation

### Android

- AudioRecord or MediaRecorder for capture
- MediaExtractor/MediaCodec or ExoPlayer for decoding
- AudioTrack/ExoPlayer for playback
- Jetpack Compose or another UI framework for presentation

### Desktop

- Platform audio APIs or a small WAV decoder
- Desktop UI toolkit of choice

Every platform should pass Float32 PCM to PitcheeCore. Do not reimplement the
mel frontend, VAD hysteresis, model windowing, or score rules on the platform
side. Timeline geometry and visual colors belong to the UI layer, not the core.

## ABI rules

- Keep `pitchee.h` C-compatible.
- HNR has its own C ABI in `include/pitchee/hnr.h` and is not included by the
  `pitchee.h` umbrella. Its implementation lives in `src/hnr.cpp`; it does not
  load a VAD or F0 model, so callers provide already-computed VAD segments.
- `pitchee_analyzer_detect_vad()` supplies VAD intervals for composition with
  HNR. The analyzer does not call HNR and the batch JSON does not contain HNR.
- Never expose `std::string`, `std::vector`, or exceptions across the ABI.
- Return errors through `pitchee_status_t` and a fixed caller buffer.
- Return dynamic results as UTF-8 JSON or opaque handles.
- Keep model paths and file decoding outside the shared ABI.
- Increment the model version whenever model assets change.

## Realtime lifecycle rules

- A realtime F0 stream owns a fixed model input shape. Recreate it when changing
  `context_samples` or `inference_hop_samples`.
- `frame_callback` is synchronous on the `process()` caller thread.
- Calls to `process()` and `reset()` for the same stream must be serialized.
- `destroy()` must not race with an active `process()` or callback.
- Call `pitchee_realtime_f0_warmup()` after creating a stream if the application
  needs to move backend shape initialization out of the recording path.
- Read model timing metadata from `pitchee_realtime_f0_get_metadata()` rather
  than hard-coding SwiftF0 sample rate or frame hop.

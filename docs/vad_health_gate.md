# The voice-health VAD gate

The health-scoring path runs the voice-activity detector at its own operating
point. This document records why, how the numbers were chosen, and what to
re-run before changing them.

## Two gates, not one

`VadDetector::detect` rejects a clip in two independent places:

1. **Silero probability.** The ONNX model emits a speech probability per 512
   samples; `speech_start_probability` / `speech_end_probability` turn that
   into raw segments.
2. **Periodicity.** `finalize_vad_result` then measures per-frame
   autocorrelation periodicity inside each raw segment and discards the whole
   segment when fewer than `minimum_voiced_ratio` of its frames reach
   `minimum_periodicity`. The counter is called
   `discarded_breath_like_count`: the gate exists to remove breath.

Both produce the same user-visible outcome — no segments — so the two were
indistinguishable from outside until `VadResult::rejection` was added.

## What was measured

296 PVQD clips were scored for the v1 paper; 7 produced no speech and were
dropped from the analysis (n = 289). Four of those 7 are clinically severe
(CAPE-V overall severity PT084 = 71.17, PT087 = 69.50, PT118 = 88.33,
PT136 = 81.50). A reviewer asked whether dropping severe clips inflated the
reported correlation.

Re-running those 7 clips through the detector with the counters exposed
(harness: `vad_eval/` in the VocalHealth-v2 repo, raw grids in
`vad_eval/sweep_period.csv` and `vad_eval/sweep_silero.csv`):

| clip | severity | Silero segments | rejecting gate |
|---|---|---|---|
| PT084_ENSS | 71.17 | 0 | Silero probability |
| PT087_ENSS | 69.50 | 1 | periodicity |
| PT118_ENSS | 88.33 | 3 | periodicity |
| PT136_ENSS | 81.50 | 5 | periodicity |
| PT132_ENSS | 14.00 | 7 | periodicity |
| PT134_ENSS | 5.17 | 4 | periodicity |
| SJ5016ENSS | 25.88 | 8 | periodicity |

**Six of the seven were rejected by our own periodicity filter, not by
Silero.** Attributing the rejections to "Silero VAD detected no speech" — as
the error string and the v1 notes did — is wrong. Severe breathy dysphonia has
little periodic energy, so the gate that removes breath removes the patients
the screen exists to find.

## The operating point

`voice_health_vad_thresholds()` in `src/internal.hpp`:

| threshold | general | health | why |
|---|---|---|---|
| `speech_start_probability` | 0.60 | 0.60 | unchanged — see below |
| `speech_end_probability` | 0.30 | 0.30 | unchanged |
| `minimum_periodicity` | 0.50 | **0.30** | recovers the periodicity-rejected clips |
| `minimum_voiced_ratio` | 0.12 | **0.08** | PT136 needs it; 0.05/0.03 change nothing further |
| `minimum_speech_seconds` | 0.0 | **2.0** | refuses clips too short to measure |

**Silero's thresholds stay at the general values.** Dropping them to 0.25/0.12
buys 0.12 s of speech on PT084 — still unscorable — and Silero is the only
gate that rejects silence, broadband noise at −40 and −20 dBFS, 50 Hz mains
hum, and breath-like filtered noise (all five synthetic controls are rejected
with `silero_segment_count == 0`, at every point in the grid). Loosening the
gate that holds back non-speech to chase one clip would trade a silent
rejection for a fabricated score.

**Why there is a speech floor.** The perturbation estimators in
`voice_health.cpp` (`local_jitter`, `local_shimmer`, `moving_perturbation`,
`median`) return `0.0` when they have too few frames instead of failing. A
clip that retains 0.09 s of speech is therefore scored from zero-filled
jitter, shimmer and CPPS, with the remaining signal coming from an ECAPA
embedding computed over mostly non-speech audio. The score looks ordinary and
means nothing. 2.0 s keeps PT118 (3.83 s retained) with margin and refuses
PT087, whose retained speech saturates at 0.55 s even at the loosest grid
point.

## Outcome on the 7 clips

| clip | severity | before | after |
|---|---|---|---|
| PT118_ENSS | 88.33 | no speech | scored, 3.83 s speech, health 1.47 (disordered) |
| PT136_ENSS | 81.50 | no speech | scored, 11.99 s speech, health 6.10 (disordered) |
| PT132_ENSS | 14.00 | no speech | scored, 13.07 s speech, health 90.46 (healthy) |
| PT134_ENSS | 5.17 | no speech | scored, 12.45 s speech, health 79.12 (healthy) |
| SJ5016ENSS | 25.88 | no speech | scored, 9.30 s speech, health 37.51 (healthy) |
| PT084_ENSS | 71.17 | no speech | `assessable = false`, reason `silero_probability` |
| PT087_ENSS | 69.50 | no speech | `assessable = false`, reason `insufficient_speech` |

All five newly scored clips land on the correct side of the v1 operating
point. Two of the four severe clips become scorable; the other two carry no
measurable voiced speech at any threshold and are now reported as not
assessable rather than dropped silently. The healthy reference clips move by
under 0.2 s of retained speech (BL02 17.76 s → 17.91 s).

## Corpus-wide outcome

All 296 PVQD ENSS clips, both profiles (`vad_eval/corpus_general.csv`,
`vad_eval/corpus_health.csv`):

| profile | accepted | rejected | rejection rate |
|---|---|---|---|
| general | 289 / 296 | 7 | 2.36% |
| voice_health | 293 / 296 | 3 | 1.01% |

The general profile's 7 rejections reproduce the n = 289 of the v1 paper
exactly, which is what validates the harness.

The loosened periodicity gate matters well beyond those 7. Under the general
profile, 9 of the 289 *scored* clips retained under 2 s of speech and 17 under
3 s, the worst being LA9021_ENSS: 0.04 s of retained speech, scored 97.21.
Those clips were scored from zero-filled jitter, shimmer and CPPS. They were
not short recordings — the periodicity gate was discarding their speech as
breath. Under the health profile the same clips retain 8.7–13.1 s, and the
minimum retained speech across all 293 accepted clips is 2.98 s.

The three clips that remain unassessable are all severe: PT008 (severity
98.67, 0.885 s retained), PT084 (71.17, 0 s) and PT087 (69.50, 0.09 s). PT008
is the one clip that loses a score it previously had; that score came from
0.86 s of speech and zero-filled features. A voice with no measurable
periodic phonation is not an unknown — it is a clinical red flag, and
"recommend a clinical review" is the correct output for it.

**The 2.0 s floor is not a knife-edge.** On this corpus the rejected clips top
out at 0.885 s of retained speech and the accepted ones start at 2.98 s, so
any floor in (0.885, 2.98] produces identical results. 2.0 s sits in the
middle of that gap.

## The assessability contract

`pitchee_voice_health_result_t.assessment` and
`pitchee_voice_severity_result_t.assessment` are populated on every call,
including the rejecting ones, and the health entry points still return
`PITCHEE_ERROR_NO_SPEECH` with the score fields zeroed. A caller that ignores
the new struct cannot read a fabricated score; a caller that reads it gets the
reason, the retained speech, and the two segment counters, which is what the
rejection-rate metric and the paper's limitations section need.

`assessable == 0` must be presented as "not reliably assessable, recommend a
clinical review". Never as a score, and never as a generic failure.

## Before changing any of these numbers

```bash
# rebuild the core and the probes
cmake --build build-vadcheck -j 8
cd <VocalHealth-v2>/vad_eval && ./run_probe.sh build

./run_probe.sh probe           # current point, 7 clips + controls + references
./run_probe.sh sweep-period    # periodicity grid at the default Silero point
PERIODICITY=0.30 VOICED=0.08 ./run_probe.sh sweep-silero
ctest --test-dir build-vadcheck -R vad_gate   # profile separation invariants
```

Update the tables above with what you measured. The four numbers decide who
gets a health score.

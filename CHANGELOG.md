# Changelog

All notable changes to this project are documented here. Versions follow
[semantic versioning](https://semver.org/); while the major version is 0 the
public C ABI may still change between minor releases.

## [0.1.0] - 2026-09-26

First release. The engine and the offline tool are tested on Linux, Windows
and macOS; the OBS plugin compiles on Linux and Windows but **has not yet
rendered a frame inside a running OBS**, which is why this is a pre-release.

### Engine

- Temporal blur engine with a stable C99 push/pull ABI. All allocation
  happens at context creation; nothing in the frame path allocates or locks.
- Two modes. **Decimate** folds each frame into a running accumulator over
  disjoint windows — one pass per input frame regardless of blur length — and
  is a true shutter integral when the source runs faster than the output.
  **Rolling** blends the trailing N frames at the same rate, with a sliding
  add/subtract fast path when every tap carries the same weight.
- Nine weighting kernels (`equal`, `gaussian`, `gaussian_sym`,
  `gaussian_reverse`, `pyramid`, `vegas`, `ascending`, `descending`,
  `custom`), each defined as a continuous shape sampled at tap centres, so
  changing the blur length does not change the character of the blur.
- Shutter angle: below 1.0 drops the outer taps (crisper, and faster, since
  those taps are skipped); above 1.0 flattens towards equal weighting for
  deliberate ghosting.
- Blending in linear light by default. Averaging gamma-encoded values
  darkens the trail; an alternating black/white sequence averages to 188,
  not 127.
- Scalar, SSE2, AVX2 and AVX-512 kernels chosen by CPUID at context
  creation. Every vectorised kernel is asserted byte-identical to the scalar
  reference, so the backend cannot change the image.
- Formats: RGBA8, BGRA8, NV12, I420. YUV refuses linear-light blending
  rather than silently shifting hues.

Measured per input frame at 1080p on a 4-core machine: NV12 decimate
0.199 ms, RGBA gamma 0.471 ms, RGBA linear 1.28 ms.

### blurcli

- Blends raw frames on a pipe, or spawns `ffmpeg` at both ends for container
  files. FFmpeg is a runtime dependency only — nothing links it.
- `--interpolate N` lifts footage to a higher rate through ffmpeg's
  `minterpolate` before blending, which is what makes 60 fps in / 60 fps out
  with a real shutter integral possible.
- Option names follow blur's vocabulary so existing configs transfer.
  Unknown values are errors, never silent fallbacks.
- `--dry-run` prints the ffmpeg commands without running them.

### OBS plugin

- Video filter offering both blur modes, with weighting, shutter angle,
  phase offset and gaussian parameters as properties.
- Accumulates on the GPU through the fixed-function blend unit into an
  RGBA16F target, sampling through an sRGB view so the blend happens in
  linear light at no ALU cost. Alpha is premultiplied before weighting.
- Drives `obs_encoder_set_frame_rate_divisor` at recording start, so a
  240 fps canvas records at 60 fps. Logs canvas versus recorded rates and
  warns when the numbers cannot produce real blur.
- Targets OBS 30.0 and loads on 30, 31 and 32.

### Known limitations

- The plugin has not been exercised inside a running OBS.
- Rolling blend is a trailing smear, not an exposure; it is labelled as such.
- "Sub-millisecond" is processing cost per frame, not end-to-end latency —
  any correct shutter integral must buffer its window.
- The offline pipeline copies frames through a pipe rather than staying on
  the GPU. The zero-copy D3D11 design is written up in `docs/design-notes.md`.

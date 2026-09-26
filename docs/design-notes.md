# Design notes

Decisions that cost research to reach, and work that is deliberately not done
yet. Recorded so nobody has to rediscover them.

## OBS: how to encode below the canvas rate

`obs_encoder_set_frame_rate_divisor(encoder, N)` is the mechanism, and it is a
first-class libobs feature rather than a trick:

- `media-io/video-io.c` decimates with an explicit counter, not a modulo, so
  encoders started together land on the same frames;
- `obs-video-gpu-encode.c` honours the divisor on the **NVENC zero-copy texture
  path** and scales the timebase accordingly, so 240 fps canvas + divisor 4 +
  GPU encoding works;
- encoder groups compute an LCM of the divisors, so a 240 fps stream and a
  60 fps recording keep their keyframes aligned.

It can only be set while the encoder is stopped, which is why the plugin hooks
`OBS_FRONTEND_EVENT_RECORDING_STARTING`.

### Dead ends

**Per-canvas frame rates do not exist.** `obs_init_video_mix()` in `libobs/obs.c`
overwrites every auxiliary mix's `fps_num`/`fps_den` with the main canvas's,
with a comment explaining that the main view's graphics thread drives all frame
output. `obs_view_add2()` and the OBS 31 canvas API give you another resolution
and scene graph, never another clock. There is one graphics thread and one
frame interval.

**A filter cannot pull extra subframes.** It may call
`obs_source_video_render(target)` several times in one `video_render`, but the
scene graph and every `video_tick` delta advanced exactly once for that tick —
you would blend the identical image with itself and gain nothing. Temporal
information originates at the source and is sampled once per graphics tick.

**A custom output or encoder wrapper is strictly worse.** You would have to
fake the timebase, you would break encoder-group alignment, and you would have
to wrap every encoder ID a user might choose — to reimplement decimation that
`video_output_connect2` already does correctly.

### Traps

- `video_render` runs once **per reference** to the source: zero times when
  hidden, and again for each of Studio Mode preview and program, every open
  projector, and the properties dialog. Only `video_tick` is once per frame.
  The `processed_frame` guard is not optional.
- `obs_source_process_filter_begin/end` cannot hold an accumulator: it owns one
  texture, and its `can_bypass()` path renders the parent directly without
  rendering to a texture at all.
- The capture pass must force `gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO)`,
  or the target is composited onto the render target instead of copied.
- Do not enable framebuffer sRGB when writing the accumulator — it already
  holds linear values, and double-linearising is the second-easiest bug to
  ship here. The easiest is not linearising at all.
- Build against the **oldest** supported OBS. libobs rejects a plugin whose
  `major.minor` exceeds the host's and logs the rejection at `LOG_DEBUG`, so
  the user's symptom is a missing filter and an empty log.

## Building the plugin for Windows without building OBS

The plugin links libobs and obs-frontend-api. Linux distributions package
both with working CMake configs — Ubuntu 24.04's `libobs-dev` is 30.0.2,
exactly the floor the plugin targets — so there `find_package` is the whole
story. Windows has no such package, and the usual answer is to build OBS from
source, which drags in obs-deps and Qt to produce a library that is linked
and never compiled against.

The shortcut rests on one fact worth checking before relying on it:
`UI/obs-frontend-api/CMakeLists.txt` links **only `OBS::libobs`**. No Qt, no
UI. So nothing about the plugin needs OBS built.

`.github/scripts/make-obs-sdk.ps1` therefore:

1. downloads OBS's official Windows release — note the archive is plainly
   `OBS-Studio-30.0.2.zip`, with no "Windows" in the name, while every other
   platform is spelled out, so match on shape rather than filename;
2. takes headers from the source at the same tag. `obs-config.h` is a plain
   in-tree header in OBS 30.x with literal version numbers, so nothing needs
   generating;
3. generates import libraries from the shipped DLLs' own export tables —
   `dumpbin /exports` to a `.def`, then `lib /def:`.

Linking against a library generated from the real DLL is ABI-correct, because
that DLL is the one the plugin loads.

Two things this cost, both worth knowing:

- **Do not include `<util/threading.h>`.** It reaches for `<pthread.h>`,
  which on Windows means OBS's bundled w32-pthreads — a separate library with
  its own DLL and import lib, which would undo the whole approach. Spell out
  the one mutex instead.
- **Each workflow step is a fresh shell.** Entering the Visual Studio
  developer environment in one step does nothing for the next, so every step
  needing `dumpbin` or `lib` dot-sources `vsdevshell.ps1` for itself.

## Accumulator precision

The CPU path accumulates in unsigned Q16 held in `uint16_t`. Weights normalise
to 1 and samples are in [0,1], so the running sum cannot exceed 1.0 and fits
exactly, at half the bandwidth of a float accumulator — which matters, because
this kernel is bandwidth-bound.

Two things make that safe:

- The per-tap tables **truncate** rather than round. Rounding eight taps of
  0.125 gives 8192 each, and 8 × 8192 = 65536, which wraps to zero: white
  frames resolve to black. Truncating guarantees
  `sum(floor(w_i · x)) ≤ floor(sum(w_i) · x) ≤ 65535`, costing at most N counts
  out of 65535 — far below one 8-bit step.
- Weights are normalised straight, with no residual pushed into the largest tap.
  Nudging one weight to make the floats sum to exactly 1 breaks the bitwise
  symmetry of a flat kernel, and rolling mode's sliding window relies on every
  tap sharing one table.

On the GPU the accumulator is `GS_RGBA16F`. 8-bit loses about three bits per add
and bands visibly; 32-bit doubles VRAM and bandwidth for a precision nobody can
see (1080p × 8 taps: 127 MB versus 254 MB). fp16 would drift in a *long-lived*
running accumulator, but each window is accumulated and reset, so it does not.

## Not done yet

### Zero-copy D3D11 offline pipeline

`blurcli` currently pipes raw frames through ffmpeg subprocesses. That is a
deliberate trade — no build dependency, the raw path is testable without a
single video file, and ffmpeg's `minterpolate` gives interpolation for free —
but it costs a copy through the pipe and a host round trip.

The zero-copy version is `AV_HWDEVICE_TYPE_D3D11VA` decode → D3D11 compute
shader → NVENC with `AV_PIX_FMT_D3D11`, sharing one device between decoder and
encoder. It is achievable; it was not written because it cannot be written
responsibly without a machine to test it on. The specifics, for whoever does:

- Set `AVD3D11VADeviceContext.BindFlags |= D3D11_BIND_SHADER_RESOURCE`
  **before** the frames context is initialised, or `CreateShaderResourceView`
  fails and you will be tempted to add a staging copy.
- Decoder output is an *array* texture: `data[0]` is the `ID3D11Texture2D*` and
  `data[1]` is the array slice index. SRVs need
  `D3D11_SRV_DIMENSION_TEXTURE2DARRAY` with `FirstArraySlice` set, cached per
  `(texture, slice)` — creating views per frame is a measurable stall.
- NV12 needs two SRVs: `R8_UNORM` for luma and `R8G8_UNORM` for chroma. There
  is no single-view NV12 sample. P010 uses `R16_UNORM`/`R16G16_UNORM`.
- Holding N frames for the blur window means the decoder pool must be at least
  N + DPB + encoder surfaces, or the decoder deadlocks waiting for a surface.
  The driver caps decoder-bound arrays at 64 slices, so cap N around 16.
- Respect the D3D11VA context's `lock`/`unlock` callbacks on the render thread;
  contention shows up as mystery hitching rather than corruption.
- Verify with a GPU capture that there is no `CopyResource` per frame. That is
  the difference between hitting and missing the budget.
- AMF and QSV do not take `AV_PIX_FMT_D3D11` as cleanly; plan an
  `av_hwframe_transfer_data` fallback and measure it, because a 1080p NV12
  readback plus upload will consume the entire budget by itself.

### A better linear-light CPU path

Linear-light RGBA runs about 2.7× the gamma path, because a 256-entry table
lookup per byte is compute-bound where the rest of the kernel is
bandwidth-bound. The table is exact — no approximation error anywhere — which
is why it was chosen, but a vectorised alternative (AVX-512 `vpermi2w` chains,
or a polynomial approximation of the EOTF with a measured error bound) would be
worth benchmarking against it.

### Interpolation quality

`minterpolate` is what ffmpeg ships and it needs nothing extra, but RIFE via
`rife-ncnn-vulkan` (MIT, redistributable) is substantially better on fast
motion. The natural shape is an auto-detected external interpolator selected by
a `--interpolator` flag, keeping the core dependency-free. Interpolation stays
offline-only regardless: nothing of that quality fits a realtime budget, which
is why the OBS side oversamples the canvas instead.

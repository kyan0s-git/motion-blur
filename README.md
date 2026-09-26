# motion-blur

Temporal motion blur for OBS Studio and for video files. One engine, two
front ends: a realtime OBS filter and an offline command-line tool.

It exists to make high-frame-rate game footage look like it was filmed rather
than sampled — the 60 fps stutter of a recording taken from a 240 fps game
becomes a smooth 60 fps with a real exposure.

## The one thing to understand first

Motion blur is the integral of the image over the shutter interval. You cannot
extract that integral from a single frame — **it needs more temporal samples
than it emits.** Everything else in this project follows from that.

There are exactly two honest ways to get those samples:

| | How | Where |
|---|---|---|
| **Oversample** | Run the source above the output rate: a 240 fps canvas blended 4:1 into a 60 fps recording | OBS plugin |
| **Interpolate** | Synthesise intermediate frames, then blend down: 60 fps → 480 fps → 60 fps | `blurcli` |

There is also **rolling blend** — a weighted average of the last N frames at the
same frame rate. It needs no setup and works anywhere, but each source frame
contributes to N output frames, so it is a trailing smear rather than an
exposure. It is offered, and it is labelled honestly.

Three consequences worth stating plainly:

- **Sub-millisecond describes processing cost, not latency.** Any correct
  shutter integral must buffer its window. Decimating 240→60 with 4 frames
  costs three source-frame periods, about 12.5 ms, before the first output
  exists. That is physics, not an implementation detail.
- **60 fps in and 60 fps out, with neither oversampling nor interpolation, can
  only give you rolling blend.** If you want a real exposure from footage
  already at your target rate, you need `--interpolate`.
- **The source has to actually be fast.** A 240 fps canvas capturing a game
  that presents at 60 fps sees each frame four times; blending four copies of
  one image returns that image. Worse, a canvas that *requests* 240 fps but
  achieves 160 produces duplicated frames at irregular intervals, and
  duplicates in an accumulator show up as periodic banding in the blur.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

The core and `blurcli` have **no third-party dependencies**. `blurcli` calls
`ffmpeg` and `ffprobe` as subprocesses when you give it a container file; it
does not link them, so nothing is needed at build time and any ffmpeg on
`PATH` will do.

The OBS plugin needs libobs and is off by default:

```sh
sudo apt-get install libobs-dev      # Ubuntu 24.04 ships 30.0.2, the version we target
cmake -S . -B build -DMBLUR_BUILD_OBS_PLUGIN=ON
cmake --build build
ctest --test-dir build               # includes a load check on the built module
```

On Windows there is no libobs package, and building OBS from source to get
one means pulling Qt and obs-deps for a library the plugin links but never
needs compiled. `.github/scripts/make-obs-sdk.ps1` avoids that: it takes the
`obs.dll` and `obs-frontend-api.dll` from OBS's own release, generates import
libraries from their export tables, pairs them with headers from the matching
source tag, and `-DOBS_SDK_DIR=` points the build at the result. Linking
against a library generated from the real DLL is ABI-correct, because it is
the DLL the plugin loads.

## blurcli

Blur footage that is already faster than you want to keep:

```sh
blurcli --input capture240.mp4 --output out.mp4 --frames 4
# 240 fps in, blended 4:1, 60 fps out, with a true 360-degree shutter
```

Blur footage that is already at your target rate — the common case for game
recordings:

```sh
blurcli --input gameplay60.mp4 --output out.mp4 --interpolate 8 --frames 8
# 60 -> 480 fps via ffmpeg's minterpolate, blended 8:1, back to 60 fps
```

Tune the look:

```sh
blurcli --input in.mp4 --output out.mp4 --frames 8 \
        --blur-weighting gaussian_sym --blur-amount 0.6
```

`--blur-amount` is the shutter angle. At 1.0 the whole window is exposed; below
1.0 the outer frames are dropped for a crisper, more strobed look (and it
renders faster, since those taps are skipped); above 1.0 the curve flattens
towards equal weighting, which is deliberate ghosting.

Weighting kernels: `equal`, `gaussian`, `gaussian_sym`, `gaussian_reverse`,
`pyramid`, `vegas`, `ascending`, `descending`, `custom`. Each is defined as a
continuous shape sampled at the tap centres, so changing the blur length does
not change the character of the blur.

Raw frames on a pipe, for anyone who wants to drive ffmpeg themselves:

```sh
ffmpeg -i in.mp4 -f rawvideo -pix_fmt nv12 - \
  | blurcli --raw --width 1920 --height 1080 --frames 4 \
  | ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -r 60 -i - out.mp4
```

`--dry-run` prints the ffmpeg commands without running anything.

## OBS plugin

Add **Motion Blur** as a filter on any source.

- **Rolling blend** works immediately with no other changes. Start here.
- **Decimate** is the real thing, and needs the canvas running above your
  recording rate. Set Settings → Video → FPS to N times the rate you want
  recorded (4 × 60 = 240), set the filter's blur length to N, and enable *Set
  the recording frame rate divisor automatically*. The plugin then calls
  `obs_encoder_set_frame_rate_divisor` when recording starts, so the canvas
  runs at 240 and the file is written at 60.

Check the OBS log after starting a recording: the plugin reports the canvas
rate, the window length and the resulting recorded rate, and warns when the
numbers do not add up.

A note on what the plugin cannot do: a filter is handed exactly one image per
graphics tick and cannot sample the timeline faster. Running the canvas faster
is the only way to feed it more, which is why the frame-rate divisor exists.
Per-canvas frame rates are not an alternative — OBS overwrites every auxiliary
mix's frame rate with the main canvas's, by design, because one graphics thread
drives all frame output.

## Performance

Measured, not estimated. Per **input** frame at 1080p, on a 4-core machine
(`./build/bench/mblur_bench`):

| Path | N=8 | Notes |
|---|---|---|
| NV12 decimate, AVX2 | **0.199 ms** | what the offline encoder path runs |
| RGBA8 decimate, gamma, AVX-512 | 0.471 ms | |
| RGBA8 decimate, linear light, SSE2 | 1.28 ms | exact transfer function, table-driven |
| RGBA8 rolling, sliding window, AVX-512 | 1.71 ms | N=16 |
| RGBA8 rolling, re-summed | 10.8 ms | N passes per output; use decimate |

The kernel is bandwidth-bound, so the only thing that helps is touching fewer
bytes. Decimate mode folds each frame into a running accumulator over disjoint
windows, which costs **one pass per input frame regardless of blur length**.
Rolling mode has to rebuild an overlapping window, and only gets back to O(1)
when every tap carries the same weight and the window can slide.

The CPU path is a fallback and a correctness reference, not the main event —
inside OBS the blend runs on the GPU, where the same work is a rounding error.

`mblur_bench --check-budgets` exits non-zero if any path exceeds its budget.
Each case carries its own: one number cannot describe both an accumulator that
touches a frame once and a kernel that re-sums N taps per output frame, and a
shared budget means either the fast path is never really checked or the O(N)
path fails for being exactly what it is. `--budget-scale` loosens them all for
slower machines.

## Correctness

Blending happens in **linear light** by default. Averaging gamma-encoded values
darkens the result, because the sRGB curve is concave: a bright object crossing
a dark background leaves grey mud instead of a bright streak. The test suite
pins this down — an alternating black/white sequence averages to 188, not 127.

Other things the tests assert, because each of them was a bug at some point:

- every vectorised kernel matches the scalar reference **byte for byte**, across
  sizes, tails and dither phases, so the backend choice cannot change the image;
- dither is deterministic by pixel position and exactly zero-mean, so thread
  count and vector width do not alter output;
- a constant input returns unchanged through every weighting kernel (this
  catches normalisation drift, a missing transfer function and accumulator
  overflow at once);
- rolling and decimate agree on an identical window;
- the steady-state frame path performs **zero allocations**, checked by wrapping
  the allocator at link time;
- the built OBS module loads: a test `dlopen`s it with `RTLD_NOW` and resolves
  the three entry points OBS looks up. A plugin links happily with unresolved
  symbols and then fails to load, which OBS reports only at `LOG_DEBUG` — so
  the symptom is "the filter isn't in the list" and an apparently clean log.
  The module is also linked with `--no-undefined` so that failure cannot leave
  the build at all.

YUV formats refuse `--linear-light` rather than pretending: blending chroma as
if it were light shifts hues on coloured motion, and converting to RGB first
would cost more than the blend. The refusal says so and suggests `--format
rgba8`.

## Layout

```
include/mblur/mblur.h   public C99 ABI (push/pull)
src/core/               weights, colour, layout, pipeline, thread pool
src/cpu/                scalar reference + SSE2/AVX2/AVX-512, runtime dispatch
obs-plugin/             OBS filter, frontend helper, accumulate.effect
apps/blurcli/           offline tool
tests/  bench/
docs/                   design notes, including work not yet done
```

## Install

Grab an archive from [releases](https://github.com/kyan0s-git/motion-blur/releases).
Each one contains `blurcli`, and the Linux and Windows archives also contain the
OBS plugin already laid out the way OBS expects — unpack `obs-plugin/motion-blur`
into `%APPDATA%\obs-studio\plugins\` (or `~/.config/obs-studio/plugins/`),
restart OBS, and add **Motion Blur** as a filter. `INSTALL.txt` in each archive
says the same thing with the exact paths.

## Status

The engine and `blurcli` are built and tested on Linux, Windows and macOS. The
OBS plugin compiles on Linux and Windows and passes a load check, but **has not
yet rendered a frame inside a running OBS** — that is the next thing to verify,
and why releases are marked pre-release for now.

See [docs/design-notes.md](docs/design-notes.md) for what is deferred and why,
and [CHANGELOG.md](CHANGELOG.md) for what is in each release.

## Licence

GPL-2.0-or-later, matching the OBS ecosystem. See `LICENSE`.

`f0e/blur` and `Wieku/danser-go` were design references for the feature set and
vocabulary. No code was copied from either; see `NOTICE`.

# Skipping the work on a repeated picture: what the Magpie fork does, and our version (spec, 2026-10-09)

**How this was made.** Read, in SAOG0721/Magpie (GPL-3.0), `src/Magpie.Core/include/EffectFrameState.h` and `src/Magpie.Core/shaders/DuplicateFrameCS.hlsl` and its review note
(`docs/experimental/reviews/20261001-v0.6.9-duplicate-frame-filter.md`). This page says what they do and why, in our words, and then what we will build. **Nothing is copied**: not the code, the shader, the
names or the structure; what is used is the idea and the algorithm, which are free. The implementation below is written from this page, not from their files.

## What they do

1. **Exact compare of two pictures on the GPU.** One dispatch. A thread group covers a block of 16 x 16 pixels; each of its 8 x 8 threads takes a 2 x 2 quad (one gather per channel fetches its four pixels from each of the two pictures)
   and compares red, green and blue exactly (alpha too in HDR, where alpha is kept). Any difference sets a flag for the group; the group publishes it into one result word with an atomic OR (the flag only goes 0 -> 1, so no ordering is needed).
   The word is cleared before each compare and read back by the CPU (a synchronous Map; an asynchronous ring was considered and not built because they had no measurement showing a gain).
   Why exact: a repeat is bit for bit the same, and "almost the same" is a different picture (a small thing moving, a cursor, a clock). Why per quad: gather makes the compare a quarter of the fetches.
2. **A version per effect output.** Each effect remembers the *key* of its last successful run: the capture sequence, the version of its input's content, the version of its input's temporal history, the version of its parameters, a resource
   generation (changes when textures are rebuilt), and the identity, size and format of its input and output textures. A new run is needed when the key differs, the effect has no valid output, or the effect needs the clock (some SMAA modes).
   A run that succeeds publishes a new output version; a reuse leaves it alone. So a repeated input leaves every version unchanged and every effect after it reuses its output; the first effect that does change (a parameter edit) makes the ones after it recompute.
3. **Two kinds of version.** The *content* version moves on every real new output; the *history* version moves only on a discontinuity (a resize, a new capture session, a format change, a parameter change that makes old history wrong), so
   ordinary new frames do not clear temporal accumulation. A temporal effect that must run again on an old input (a minimum frame rate, a cursor refresh) gets a zero-motion guide and its SDK's reset flag so that the repeat is not taken as new content.
4. **Failures are not cached**: an output that failed to draw is never reused.
5. They log a reason for every frame (accepted, repeat, interrupted, new sequence, colour change, readback failed, ...).

## Why it matters here

- The expensive things in this project run once per picture: Neural Rendering 8-12 ms at 4K, the upscalers 1.4 ms (2560x1440) to 4 ms (5120x2160) per presented picture (docs measured 2026-10-09).
  A 30 fps source in a 60/120 Hz feed repeats each picture 2-4 times; a pause menu, a loading screen or an idle desktop repeats for as long as it lasts. All of that is work with an identical result.
- Our repeat note (source_cadence.cpp) compares 2,304 points with a tolerance. That is right for *noticing* a regular pattern and wrong for *acting*: a small moving region between the points would be frozen. Acting needs the exact full-frame compare.

## What we will build

Differences from theirs, because our host is not theirs: **we run inside Lossless Scaling's render thread, so the compare's answer must not be waited for there**; our engines are D3D12 queues of their own, fed by shared textures and fences.

- **Compare on the engine's queue, in its command list**: the engine already copies the frame in; one more dispatch compares it with the previous input (exact, whole frame, a flag in a small buffer).
- **Skip on the GPU, with predication** (D3D12 `SetPredication` from the flag buffer): the upscale / model work in that list runs only when the picture differs; when it repeats, a copy of the last output into this picture's slot runs instead. No CPU wait, no latency.
  The open question is NVIDIA's, AMD's and Intel's runtimes recording their dispatches under a predicate; measured first offline with `nr_sreval dup=N` (it already feeds repeats and has a GPU-time bench).
- **If predication is not safe with a runtime**: the delayed flag (read a few frames late, never waited for, as the repeat note does now) plus the stable repeat count and phase from the detector: run on the first picture of each group and reuse for the rest.
- **History**: a repeat does not advance the temporal history (the next real picture sees one picture fewer, as if the repeats had not been shown); a resize or a settings change resets it as today.
- **Off by default** until it has been seen live. Cost to watch: the compare itself (a 4K frame, one gather per quad per channel: expected well under 0.1 ms).

## Measurements to make (offline, on the card)

1. The compare alone: GPU ms at 2562x1442 and 3840x2160 (SDR and fp16).
2. A run with every picture new (predication present, never skipping): must cost the same as today plus the compare.
3. A run with `dup=2` and `dup=4`: the GPU time per picture, and the output against the unskipped run (must be identical on the repeats).
4. A moving small region in an otherwise repeated picture: must not be skipped (the reason for the exact compare).

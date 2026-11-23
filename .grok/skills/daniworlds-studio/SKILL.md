---
name: daniworlds-studio
description: >
  DaniWorldsStudioDesktop, the Windows C++/SDL2/ImGui/OpenGL/FFmpeg video editor
  at github.com/danimanhillel/DaniWorldsStudioDesktop (local mirror /tmp/dws).
  Open this BEFORE any edit, bugfix, export, preview, timeline, RIFE, BasicVSR++,
  Whisper, green-screen, transform, undo, or build talk about that editor.
  Triggers: DaniWorlds, DaniWorldsStudio, DaniWorldsStudioDesktop, "העורך",
  טיימליין, ייצוא, RIFE, BasicVSR, onnxruntime, preview jump, ripple, solids.
metadata:
  short-description: "DaniWorlds desktop editor: invariants, export, git, build"
user-invocable: true
---

# DaniWorldsStudioDesktop

This is a **Windows desktop video editor**, not the TanStack app in `/workspace`.
When the user is talking about this editor: do **not** scaffold a web app, do
**not** start the preview on port 8080, do **not** `npm install`, and do **not**
apply app-builder skills (`design-ui`, `building-games`, `auth`, `neon`, `og`).

Reply to the user in **Hebrew**, short. If the code changed, end with the usual
Windows build commands (below). If nothing in the exe changed, say so and do
not tell them to rebuild.

## Where the code is

| | |
|---|---|
| Remote | `github.com/danimanhillel/DaniWorldsStudioDesktop`, branch `main` |
| Local mirror | `/tmp/dws` |
| Skill written against | `39424d8` — BasicVSR++ upscale before RIFE |

Sources: `src/main.cpp` (almost all UI, timeline, preview, export), `effects.cpp`,
`overlay_fx.cpp`, `rife.cpp` / `rife.h`, `vsr.cpp` / `vsr.h`, `asr.cpp` / `asr.h`,
`onnxruntime_c_api.h`. CMake: C++17, SDL2 **2.30.8** static via FetchContent,
ImGui **v1.91.5**, OpenGL, FFmpeg only if `FFMPEG_INCLUDE_DIRS` + `FFMPEG_LIBRARIES`
(or pkg-config). MSVC flags: `/bigobj /Zm300`.

**Do not run `cmake --build` in this Linux sandbox.** There is no MSVC. A
`g++ -std=c++17 -fsyntax-only` of `vsr.cpp` / `rife.cpp` is the only local check,
and it does **not** replace the user's build. `main.cpp` is Windows-shaped
(FFmpeg + GL + ImGui) and will not link here.

## How the user builds (their machine)

They already have a build tree. Tell them, when code changed:

```
taskkill /F /IM DaniWorldsStudioDesktop.exe
git pull
cmake --build . --config Release --target DaniWorldsStudioDesktop -- /m:1
```

`/m:1` because `main.cpp` is one huge translation unit. If the exe is running,
the link fails with **LNK1104** — they must `taskkill` first. Do not invent a
new generator or a clean rebuild unless they ask.

## Git

- Author **and** committer: `danimanhillel` / `danimanhillel@users.noreply.github.com`.
- Put `GIT_AUTHOR_NAME`, `GIT_AUTHOR_EMAIL`, `GIT_COMMITTER_NAME`,
  `GIT_COMMITTER_EMAIL` on the **`git commit` command itself**. An env prefix
  on an earlier command in an `&&` chain does **not** reach `git commit`.
- **Never** `git config` (no `--global`, no local).
- Push to `origin main` works from this sandbox. Do it after a real change.
- Do not commit model weights, `onnxruntime.dll`, or `.pth` files.

## What not to do

- Do not add a feature they did not ask for. A bugfix does not get a refactor.
- Do not redo a shipped commit (list at the bottom). Read the code first; the
  summary in chat can be stale relative to `main`.
- Do not vendor `basicvsrpp.onnx`, RIFE onnx, or ORT DLLs. CMake `POST_BUILD`
  copies **only** `models/mel80.bin` and `models/multilingual.tiktoken` next to
  the exe. The build does **not** download models or DLLs.
- Do not silently bilinear-upscale when BasicVSR++ or the DLL is missing.
- Declare functions and globals **before** use. MSVC is not gcc: `C2065` /
  `C3861` (`gSolidTex`, `ClipBelow` — fixed in `33115fc`). `AddClipOnTrack` must
  **not** call `SelectOnly` (defined later). Callers `SelectOnly` after the drop.
- `ReleaseSidePlayer` closes `playerB` only. It must **not** clear
  `playingClipUnder`. That is `ReleaseUnder`.
- `main()` opens at `int main(int argc, char* argv[])` and its closing brace is
  **before** the trailing `#ifdef _MSC_VER` / `#pragma optimize("", on)`.
  A brace counter that treats the letters `str` as a string delimiter is wrong;
  only a real quote toggles string mode.

## Tracks and pictures

`kGsTrack = 6`. Labels: `V1 V2 V3 A1 A2 A3` plus a GS lane.

| Index | Name | Role |
|---|---|---|
| 0 | V1 | Visually **front**. User says CHANNEL 1. |
| 1 | V2 | Under V1. CHANNEL 2. |
| 2 | V3 | Under V2. |
| 3–5 | A1–A3 | Audio. Same selection vector as video. |
| 6 | GS | Green screen. Not a video composite track. |

Lowest video index wins visually. `ClipBelow(above, t)` is the next video clip
under `above` at time `t`. There is a **forward declaration** (export calls it
before the definition). Do not delete the forward declaration.

Preview routing (`RoutePreviewClip` / `TopPictureAt`) runs **after** `ClipBelow`.
`topClip`, `driveClip`, `previewPlate` are locals in `main` before the frame
loop. `fxOther`, `fxProgress`, `fxActiveId` are globals. Incoming transition
clip (`tb`) stays on the main player; outgoing (`ta`) on `playerB`; the picture
under a gap is `playerUnder`. Draw a texture because `playingClip == clip`,
**not** because `topClip == drawA`.

`gRam.Present` on the main player accepts 0.20s. `SyncSide` is tighter (0.045s).

## Preview cache (`PreviewRam gRam`)

- Estimated proxy size `<= 200MB` (`200ull << 20`), or an image: cache the
  **whole** clip, but only when the clip is near the playhead (1.5s behind,
  8s ahead).
- Larger clips: only a window around the playhead (0.5s behind, 4.5s ahead;
  source span capped ~6s).
- Hard cap **1GB** (`1024ull << 20`) plus `RoomFor`: leave at least 2GB physical
  RAM free (or 1/5 of RAM, whichever is larger).
- Solids are not cached. Audio tracks are not cached.
- `gRam.Tick` is skipped while export or ASR is running.

## Panels vs timeline (`b588e7a`)

Panels must stay **above** the timeline and must not move clips while a slider
is dragged.

- `io.ConfigWindowsMoveFromTitleBarOnly = true`.
- The Editor window is `NoBringToFrontOnFocus` (also no title, no resize, no move).
- `UiShield` / `CaptureUiShield` / `RestoreUiShield` and `panelPointerLock`.
  While a panel holds the mouse, timeline drags do not start.
- Child windows are named `Editor/...`, so `ForeignName` is **false** for them.
  `TimelinePointerBusy` skips child windows and does not treat `Editor*` as foreign.
- `PanelUnderPointer` is exactly: Transform, Green screen, Overlay, Bus grade,
  Subtitle, Subtitle style, Audio Effects, Clip color, Library clip.
  **Do not** add menu popups to that list.
- The preview FRAME scissor is static. Do not let a floating panel change it.
- BUS has **no "+" button**. Clicking an empty bus lane creates a segment that
  spans the full timeline range.
- A transition runs when the **left** clip is the one visually above the clip
  that continues (not "whichever track index is higher").
- **Solids** bin: black, letterbox bars, green. Drag onto video tracks and onto
  GS. Textures are `gSolidTex[3]`, created before first use. `MakeThumbnail` of
  a solid must see `gSolidTex` already declared.

## Transform drag in the preview (`bef6686`)

Only while the Transform panel is open **and** the clip is on screen
(`XformOnScreen`).

| `xformDrag` | Handle |
|---|---|
| 1 | Move |
| 2 | Bottom-right scale |
| 3 | Top rotate |
| 4 | Right-edge aspect |

`xformHolds` is true when the panel is hovered, **or** when
`ActiveIdWindow` is that Transform window and any item is active. Clear
`xformDrag` in `ClearTimelineDrags`, when the panel closes, and on mouse-up.

`ClearTimelineDrags` also clears the marquee and the group-drag vectors. It
does not need to clear `groupAnchor` (empty vectors are enough).
`TimelinePointerBusy()` calls `ClearTimelineDrags` — it must **not** clear the
marquee every frame just because the pointer is over the timeline. The marquee
starts on empty-lane mouse-down and is applied **after** the track loop.

## Underlay when the front clip does not cover (`e16207e`, `64bdbe3`)

If a transformed (or letterboxed, or effect-scaled) front clip does not fill
the frame, the clip underneath shows in the preview **and** in the export.
Identity full-frame stays on **one** decoder. Three video tracks max, so one
extra player (`playerUnder`) is enough.

`ClipCoversFrame`: offset, rotation, or
`letterbox * gsScale * gsAspect * effectScale` fails to fill
`AspectValue(exAspect)` within **0.995**. `1376/768` vs `16/9` can fail that
test. A vertical clip in a 16:9 frame does **not** cover — pillarbox underlay
is intentional.

`FrontLeavesGaps` during a transition returns true if **A or B** leaves gaps
at the current effect scale **or** at rest scale 1. If `nb == 0`, it still
returns false: two-clip transitions skip the underlay **export** path.

Export, last overlap frame, snaps to the survivor:

- B still alive → `tp = 1`
- only A alive → `tp = 0`
- both alive → `tp = 1`

Alive test uses a **float** end, then a double compare:

```cpp
float end = start + duration;
bool alive = nextT >= start && nextT < (double)end;
```

Do not write `nextT < start + duration` as a double sum; that was the export
jump at the end of a transition.

Gaps sampler: `fabs(scale - 1) <= 0.0008` direct-copies the pixel **including
alpha**. Otherwise UV is `u * w` / `v * h`, **not** `u * (w - 1)`.
`effects.cpp` `SampleScaled` is:

```cpp
int x = (int)(u * (float)w);
int y = (int)(v * (float)h);
```

Preview reveal inside `BlitFxPair` only when
`haveB && bgShowN > 0 && previewFrameValid &&` stencil bits `> 0`.
`SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8)` **before** `SDL_CreateWindow`.
`revealTex` is declared in the outer `if`, not inside the stencil block
(it is used later). Cap the `glGetError` drain at 8.

`BlitFxPair`: letterbox each texture on its own, then
`PictureCorners(gs * effect scale)`. `recordFit` stores A's letterbox for GS.
At identity gs and effect scale 1 this matches `BlitVideo`. Export
`FitInside` letterboxes each clip into the full canvas, then `FxComposite`.

Zoom In: `scaleA = 1 + 0.35p`, `scaleB = 1`.
Zoom Out: `scaleA = 1`, `scaleB = 1.35 - 0.35p`.
At `p = 1` both visible scales are 1.

## Preview handoff at the end of a transition (`bef6686`)

After `SyncPreview` advances time, call `RoutePreviewClip` **again** so the
post-step top clip is not stale. Do **not** call `SyncPreview` twice. Do
**not** point the decoder at the clip that just ended.

## Selection, group drag, delete (`2de415a`)

One `selection` vector plus `selectedClip` for video, audio, **and** GS.
Subtitles: `selectedSub` plus `subSelection`.

- Clicking an **already selected** item must **not** `SelectOnly` (that was
  breaking group drag).
- Clicking an unselected item still selects only that one.
- Trim stays single-item.
- A vertical track change applies only to the grabbed clip (`draggingClip`).
- `SnapTime` / `SnapStart` skip indices in `groupSkip`.
- Clamp the group so nothing starts before 0 (`ApplyGroupDelta`).
- Empty SUBS click **without** a drag still creates a subtitle.
- Delete removes selected **clips and** selected subs. `WantTextInput` blocks
  delete. Media-panel delete still wins over timeline delete.
- If `selectedBus >= 0`, the bus wins (do not also delete clips).
- Right-click a GS clip: `SelectOnly` and open the GS panel.
- Right-click a subtitle: single-select and open style.
- Shift-click `ToggleSelect` still clears `subSelection`. Leave that.

Group drag globals live next to `tlTrackH`: `groupClipDrag`, `groupSubDrag`,
`groupSkip`, `groupAnchor`, type `DragOrig {idx, orig}`.
`CaptureGroupDrag` after selection is finalized on mouse-down.
`ApplyGroupDelta` writes `start` from `orig + delta` every frame.
`ClearGroupDrag` on clip/sub mouse-up. If both vectors are empty, the leader
alone is dragged.

## Ripple insert (`c267508`)

Only a **new** clip dropped **between two clips on the same channel** ripples.
Moving an existing clip does **not**.

- The new clip sits at the **end of the left clip** (`at = endL`).
- The next clip on that channel sticks flush to the inserted clip.
- Everything else with `start >= at - 0.001` (video, audio, subs, GS, BUS,
  overlays) moves by the **same** delta
  `(at + dur) - rightStart`, with `dur = max(0.05, duration)`.
- If they were already touching, delta equals duration (old behavior).
- If the gap is larger than the new clip, delta is **negative** (the tail
  slides back) but `PushAfter` clamps to `floorT` (`at`) and to 0, so nothing
  enters the left clip.
- `PushAfter` runs **before** the new clip is pushed into `clips`.
- The between-test uses **raw mouse time**, not the snapped time.
- Wired on GS media/solid drop and on V/A track drop.

```cpp
float dur = std::max(0.05f, c.duration);
float delta = (at + dur) - rightStart;
PushAfter(std::min(at, rightStart), delta, at);
c.startTime = at;
```

Undo already covers this: `UndoTick` snapshots on mouse-up and coalesces while
the button is down. Do not add a second undo path for ripple or group drag.

## Undo and REC

- Ctrl+Z and Edit → Undo. `undoStack` holds **20** gestures (`pop_front`).
- While a text box is focused, ImGui keeps its own undo. Do not steal that.
- `UndoTick` does nothing while ASR or export is active.
- Effects menu: **REC** next to DELETE. If playback is running and an effect
  parameter changes, REC stores the sequence. A reset next to the effect
  clears that effect's recorded changes (not the whole timeline).

## Export size (`717cc4a`)

`exRes == 1` is **1080p** and stays the default. The `1376x768` radio is
`exRes == 5`. Do not renumber the radios.

`ExportDims(aspect, res, w, h)` when `res == 5`:

| `exAspect` | Output |
|---|---|
| 1 or 4 (9:16, 3:4) | 768 × 1376 |
| 2 (1:1) | 768 × 768 |
| else (16:9, 4:3) | 1376 × 768 |

Other res values 0–4 are 720 / 1080 / 1440 / 2160 / 4320. 16:9 widths
1280 / 1920 / 2560 / 3840 / 7680. Even dimensions (`&= ~1`).

## Export pipeline

`ExportSession::Begin(out, ew, eh, fps, exRife, exSubMode != 1, exVsr)`.

Phases:

0. Render the timeline into an mp4 at the encoder size.
1. RIFE (`BeginRifePass` / `StepRife`), only if `exRife` and fps `>= 48` and even.
   The render is at `fps/2` into `outPath + ".rife-tmp.mp4"`, then RIFE writes
   the final file at `outFps`. `RifeRun` **refuses** `w` or `h` `> 3840`.
2. BasicVSR++ (`BeginVsrPass` / `StepVsr`) when `exVsr`. This phase runs
   **before** phase 1. UI treats `phase == 1 || phase == 2` as the GPU step
   (max 16 steps / 80ms).

Order when both checkboxes are on:

1. Render **small** (scale 4 if both full dims `% 4 == 0`, else scale 2;
   fail if a side would be `< 16`).
2. Phase 2 upscales those frames to the chosen `ExportDims`.
3. `VsrShutdown` + `ReleasePassDecoder`, **then** `BeginRifePass` if `exRife`.
4. Subtitles burn on the **last full-resolution encode**, never on the low-res
   render.

Burn rule on the phase-0 render: `burnSubs && !rifeOn && !vsrOn`.
During the VSR encode, burn only if `!rifeOn`. If RIFE follows, burn inside
`StepRife`.

`CloseAll` removes both temp files (`*.rife-tmp.mp4` and `*.vsr-tmp.mp4`),
calls `VsrShutdown()` then `RifeShutdown()` if RIFE was on, and clears the VSR
fields. VSR and RIFE each `LoadLibrary` / `FreeLibrary` their **own**
`onnxruntime.dll`. They must not be live at the same time. VSR shuts down
before `RifeStartup`.

8K **and** both options is rejected in `Begin`, before the long render:

> Frame interpolation stops above 3840, so it cannot follow BasicVSR++ at this size. Uncheck it.

8K encoder inside `BeginVsrPass` uses `hevc_nvenc` / `libx265` when `w` or `h`
`> 4096`. `BeginRifePass` itself is still h264. 8K+RIFE without VSR was
pre-existing; the hard reject is only when VSR is also on.

Audio is packet-copied from the low-res file into the upscale file (same
pattern as `BeginRifePass`), then RIFE copies it again.

Missing model or DLL: fail **immediately** with `VsrProbe().detail`. Do not
start the render. If the graph's scale hint (or the first tile's measured
scale) is not the render scale, fail with a message to put a matching-scale
model in `models\basicvsrpp.onnx`. **No bilinear fallback.**

Tiling (`BuildVsrTiles`): if an LR side is `> 640` or the area is `> 640*360`,
tiles of 256 (multiple of 4) with overlap 16. Otherwise one tile, padded to a
multiple of 4 and cropped back. One UI step is one tile.

Temporal window: the model's fixed `T` if the graph has one (reject `T > 31`
at startup), otherwise 7. Keep the center frame (`T/2`). Pad the edges by
repeating the first or last frame.

Checkbox order in the export popup: **BasicVSR++** immediately **before**
"Frame interpolation", and the checkbox shows `VsrProbe().detail`.

### ONNX layout the loader accepts (`vsr.cpp`)

One float image tensor, rank 4 or 5, optional scalar scale.

| Rank | Test | Layout |
|---|---|---|
| 5, dim2 == 3 | OpenMMLab `lqs` | NTCHW `[1,T,3,H,W]` |
| 5, dim1 == 3 | | NCTHW |
| 5, dim4 == 3 | | NTHWC |
| 5, all dynamic | assumed | NTCHW |
| 4, dim3 == 3 | | NHWC (T = 1) |
| 4 otherwise | | NCHW |

Output spatial size is the axis pair that is the same integer multiple (1–8)
of the tile. Center time index is `T/2` when the output has a time axis.
Float peak `> 1.5` is treated as 0–255.

Files probed **next to the exe** (not in the repo):

- `models\basicvsrpp.onnx`
- `models\basicvsr_pp.onnx`
- `models\BasicVSR++.onnx`
- `basicvsrpp.onnx`
- `basicvsr_pp.onnx`
- `onnxruntime.dll`

Log: `vsr.log` next to the exe. CUDA preload goes through `RifePrepareCuda`
→ `PreloadCuda` and also writes `rife.log`. Do not `FreeLibrary` RIFE's
`gKeep`. If CUDA fails, the session falls back to CPU and the status says CPU.
Linux stubs say "BasicVSR++ is available on Windows".

### Models and DLLs — what is actually downloadable

- The build downloads **neither** the ONNX nor the DLL.
- If RIFE already runs, **reuse** the `onnxruntime.dll` already next to the
  exe (and `onnxruntime_providers_cuda.dll` /
  `onnxruntime_providers_shared.dll` if they placed them). No second DLL.
- There is **no** official `basicvsrpp.onnx`. Do not invent a Hugging Face or
  GitHub link for one.
- The published weights are PyTorch `.pth` (input `lqs` `[n,t,3,h,w]`, output
  4×). Renaming `.pth` to `.onnx` does **not** work. The editor only loads ONNX.
- Verified weight file (HTTP 200, **not** loadable by the checkbox):
  `https://github.com/HolyWu/vs-basicvsrpp/releases/download/model/basicvsr_plusplus_c64n7_8x1_600k_reds4_20210217-db622b2f.pth`
- Do **not** send guessed ONNX Runtime zip URLs. `v1.30.1` win-x64
  `gpu_cuda13` and the matching CPU zip both 404'd, and
  `api.github.com/repos/microsoft/onnxruntime/releases/latest` also 404'd.
  The user already has CUDA 13 + cuDNN + `zlibwapi` working for RIFE.
- Do not start a `.pth` → ONNX conversion unless they ask.

### RIFE (already shipped)

Windows-only, `onnxruntime.dll` loaded dynamically, model
`models/rife49.onnx` or `rife.onnx`. CUDA via `PreloadCuda` (zlibwapi, CUDA 13,
cuDNN). Log `rife.log`. `RifePrepareCuda` is the export VSR calls; it must
keep calling `PreloadCuda` and must not free `gKeep`.

### Whisper subs (already shipped)

`asr.cpp`. The only models the build copies are `mel80.bin` and
`multilingual.tiktoken`. Do not drop that `POST_BUILD`.

## Shipped commits — do not redo

Newest first, all on `origin/main`:

- `39424d8` BasicVSR++ checkbox, upscale before RIFE
- `c267508` ripple: next clip sticks, same delta for the tail
- `2de415a` group-drag video + audio + subs, ripple insert
- `64bdbe3` transition-end jump (export) + one-type rubber band (later opened to all types)
- `e16207e` show the clip underneath a transformed front clip
- `bef6686` preview jump at transition end + transform drag in the preview
- `33115fc` declare `gSolidTex` and `ClipBelow` before use
- `b588e7a` panels above the timeline, Solids bin, no BUS "+"
- `9ab224f` undo, 20 steps
- `d5996a4` REC
- `717cc4a` 1376×768
- `c2b16f6` / `1ed3cfb` preview RAM cache

## Before you say a code change is done

- Diff is limited to the request. No drive-by edits in `main.cpp`.
- The invariants in this file still hold. If a change touches preview,
  transitions, underlay, group drag, or ripple, re-read that section and the
  functions it names.
- Commit with the author env on the commit command, then `git push origin main`.
- Hebrew summary + the three build commands, only if the exe actually changed.

---
name: DaniWorldsStudioDesktop
description: >
  DaniWorldsStudioDesktop, the Windows C++/SDL2/ImGui/OpenGL/FFmpeg video editor
  at github.com/danimanhillel/DaniWorldsStudioDesktop (local mirror /tmp/dws).
  Open this BEFORE any edit, bugfix, export, preview, timeline, RIFE, SwinIR,
  Whisper, green-screen, transform, undo, or build talk about that editor.
  Triggers: DaniWorlds, DaniWorldsStudio, DaniWorldsStudioDesktop, "העורך",
  טיימליין, ייצוא, RIFE, SwinIR, BasicVSR, onnxruntime, preview jump, ripple, solids.
metadata:
  short-description: "DaniWorlds desktop editor: the app, the decisions, the rules"
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
| Skill written against | `a110675`. Channel cuts must not open a decoder or reseek audio. |

Sources: `src/main.cpp` (almost all UI, timeline, preview, export), `effects.cpp`,
`overlay_fx.cpp`, `rife.cpp` / `rife.h`, `asr.cpp` / `asr.h`,
`onnxruntime_c_api.h`. CMake: C++17, SDL2 **2.30.8** static via FetchContent,
ImGui **v1.91.5**, OpenGL, FFmpeg only if `FFMPEG_INCLUDE_DIRS` + `FFMPEG_LIBRARIES`
(or pkg-config). MSVC flags: `/bigobj /Zm300`.

**Do not run `cmake --build` in this Linux sandbox.** There is no MSVC. A
`g++ -std=c++17 -fsyntax-only` of `rife.cpp` is the only local check,
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
- Do not vendor RIFE onnx or ORT DLLs. CMake `POST_BUILD`
  copies **only** `models/mel80.bin` and `models/multilingual.tiktoken` next to
  the exe. The build does **not** download models or DLLs.
- **SwinIR is gone.** `swinir_realsr_x4.onnx` is a fixed 64×64 tile, so a 2K
  export was ~209 GPU calls per frame and the user rejected it as impractical.
  Do not add SwinIR or BasicVSR++ back unless they ask. There is still no
  official BasicVSR++ ONNX. `vsr.cpp` / `vsr.h` / `RifePrepareCuda` are deleted.
- Declare functions and globals **before** use. MSVC is not gcc: `C2065` /
  `C3861` (`gSolidTex`, `ClipBelow` — fixed in `33115fc`). `AddClipOnTrack` must
  **not** call `SelectOnly` (defined later). Callers `SelectOnly` after the drop.
- `ReleaseSidePlayer` closes `playerB` only. It must **not** clear
  `playingClipUnder`. That is `ReleaseUnder`.
- `main()` opens at `int main(int argc, char* argv[])` and its closing brace is
  **before** the trailing `#ifdef _MSC_VER` / `#pragma optimize("", on)`.
  A brace counter that treats the letters `str` as a string delimiter is wrong;
  only a real quote toggles string mode.
- Never name a variable `far` or `near`. Windows headers `#define` both.
  `bool far = ...` became `bool = ...` (`C2513` at the old line 6367, fixed in
  `18eff73` by renaming to `behind`).

## What the user decided

The user writes in Hebrew and says CHANNEL 1/2/3 for V1/V2/V3. They judge
playback by feel: a held frame, a jump, or a few milliseconds of silence at a
cut is a bug. They send screen recordings. The freeze they showed was at the
end of an 8s clip on V3 while V2 was the picture on screen. The preview status
changed from `RAM … cached` to `CPU … cache`. That class of hitch is still
the open problem they care about most. Do not tell them it is fixed unless a
new recording shows the cut is smooth.

In order, they asked for and accepted:

1. Full RAM cache of a clip near the playhead when the proxy is about 200MB
   or less. Larger clips keep a window, not the whole file. Cap 1GB.
2. Export size 1376×768, landscape and portrait (`exRes == 5`).
3. **REC** beside DELETE on an effect: while playing, parameter changes are
   stored as a sequence. A reset next to that effect clears only its sequence.
4. Undo, 20 steps, Ctrl+Z. Not while a text box is focused.
5. Open panels must not move the timeline under them. Static preview frame.
   No "+" on the BUS lane. A Solids bin (black, letterbox, green).
6. No jump at the end of a transition, in preview or export. Transform handles
   in the preview while that panel is open.
7. If a transformed clip does not fill the frame, the clip underneath shows,
   in preview and in export.
8. Marquee selection. It started as one type at a time, then they asked to
   drag video, audio, and subtitles together.
9. A new clip dropped between two clips on the same channel sits at the end
   of the left one and pushes everything after it by the same delta. The next
   clip stays flush.
10. They tried BasicVSR++ and then SwinIR (`swinir_realsr_x4.onnx`) as an
    upscale before RIFE. SwinIR was too slow (about 209 tiles per 2K frame)
    and they said to remove it. Do not put either back.
11. Images dropped on the GS channel keep their original aspect. Video and
    solids on GS still fill the frame.
12. A multi-selection can move up or down a channel when that channel exists.
    The click selects the thing under the pointer. Right-click Mute, Fade,
    and Speed apply to every selected clip.
13. More seconds of cache ahead, because playback and export were hitching.
    An empty click unselects only when more than one object is selected.
    Dropping an effect selects it and does **not** open its side panel.
14. A cut between channels (1↔2, 2↔3, 1↔3, any pair) must be seamless. The
    end of a clip that is underneath the one on screen must not freeze the
    picture. Audio of a clip that has ended must not seek back to the start.
15. The clip right-click **Effects** menu stays gone. Do not put that menu
    back. A transition is not applied to every overlap. The **Transitions**
    tab (after Solids, child `libtrans`) is the only way in: select two clips
    that overlap on different video channels, then drag the card onto V1, V2,
    or V3 (`TRANS` payload, index into `FxAt`). `ApplyFxToSelection` stores
    `fxOut` on the lower-track clip and `fxDur` as the overlap length.
    `TransitionForClip` returns true only when that pair has a known `fxOut`
    and `t` is inside `[overlapStart, overlapEnd)`. A is the earlier clip,
    B the later one. At `t >= overlapEnd` it returns false and the survivor
    plays on the normal player. `drive` stays on B for the whole overlap so
    the main decoder does not seek at the end. `GainAt` equal-power
    crossfades only A and B during that span (`cos` / `sin` of `p * pi/2`).
    Other overlaps are unchanged. Cards: Cross Dissolve (smoothstep), Blend
    (linear), Dip to Black, Dip to White, Wipe Left / Right / Down / Up,
    Zoom In, Zoom Out. Wipe is `FxLook.wipe` 1 right, 2 left, 3 down, 4 up.
    Dip uses `dip` plus `dipR/G/B`. Opacity is Left Shift on the
    clip-body edge, and it is the opposite of the earlier inverted ramp:
    the left edge rises from the bottom (0) to the top (100) at the handle
    and the body stays at 100; the right edge falls from the top to the
    bottom at the tail. Volume is a separate curve, one yellow line on that
    clip's audio strip: silence at the outer edge, up to the envelope at
    the fade handle, and the straight envelope is not drawn inside the
    fade. Ctrl + drag an edge of the open strip. The strip stays closed
    until the arrow in the middle of that clip is clicked. Tracks grow
    only while a strip on them is open, and the timeline scrolls vertically.
    The zoom fader sits on the right, immediately left of the aspect combo.
    The Effects library is the overlay set dragged onto the FX lane. Kinds
    stay append-only (`OverlayKindCount` is 26). 3 and 4 are already Zoom In
    and Zoom Out. 19–25 are Dust, Cassette, Frame, Film Dust, Retro Dust,
    Old Film, Record. Preview is the GL shader; export is `FilmGrade` in
    `OverlayStamp`. Do not renumber kinds. The library grid shows 19–25,
    then Zoom In / Zoom Out, then the older effects. The Media / Effects /
    Solids / Transitions tab bar does not scroll. Each tab's cards sit in
    their own child (`libmedia`, `libfx`, `libsolids`, `libtrans`). The timeline ruler stays pinned.
    Vertical scroll is only `tlbody` (the channels). Horizontal scroll is
    that same body; the ruler copies it with `SetNextWindowScroll`. Do not
    put the ruler back inside the vertical scroller. Effects cards show the
    bundled plate (`fx_plate.cpp`, the mountain image) through the real
    preview shader (`OverlayPreviewTick`, amount 0.85, ping-pong 3s).
    `DrawFxIcon` is only the fallback if that plate did not upload. Do not
    put the flat icons back while the plate is there. Transition cards play
    the real effect between two bundled plates (`trans_plates.cpp`): snow is
    A, green is B, ping-pong over 3s. `DrawTransitionCard` uses `FxEvaluate`
    (dissolve alpha, wipe clip, zoom UV, dip). The orange and blue blocks
    are only the fallback if those plates did not upload. Do not put the
    blocks back while the plates are there. `libtrans` keeps
    `AlwaysVerticalScrollbar`, so the card grid does not jump between two
    widths when the scrollbar appears and disappears.

They build on Windows themselves. The Linux sandbox has no MSVC. After an
exe change, the reply ends with `taskkill`, `git pull`, and
`cmake --build . --config Release --target DaniWorldsStudioDesktop -- /m:1`.
If only this skill changed, do not tell them to rebuild.

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

## GS images keep their aspect

Imported stills (`type == "image"`, not a solid) that **enter** the GS lane are
contained in the project frame. Green-screen **video** and **solids** still fill.
Do not letterbox at draw time: `BlitKeyed` / `BlendChromaPlaced` stretch the
texture across the dest box, and video-track images already letterbox in
`FitAspectBox` before the gs transform. Baking a contain onto a clip that stays
on V1/V2/V3 would letterbox twice.

`ContainGsImage` runs on a GS drop and when a dragged clip's track **changes
onto** GS, and only if Size/Aspect are still identity (`gsFit != 2`):

```
F = AspectValue(exAspect)
I = media.aspect (else thumbW/thumbH)
rel = clamp(I / F, 0.25, 4)
gsAspect = rel
gsScale  = rel > 1 ? 1/rel : 1
gsFit = 1
```

Displayed aspect is `F * gsAspect`. Wider than the frame letterboxes; taller
pillarboxes. `gsFit`: 0 legacy, 1 this auto contain, 2 the user moved Size or
Aspect (slider or the preview scale/aspect handle). It is the last integer on
the `K` line. Old projects omit it.

- Leaving GS resets scale/aspect to 1 when they still equal that contain, so V1
  does not letterbox twice. Position and rotation stay. A fill the user set
  (`gsFit == 2`, Size 1 and Aspect 1) is kept, including after save.
- Do **not** rebake every frame.
- `LoadProject`, after `MakeThumbnail`: GS stills with `gsFit == 1`, or legacy
  identity (`gsFit == 0` and scale/aspect still 1), are contained once.
  `gsFit == 2` is left alone.
- Changing `exAspect` refits only `gsFit == 1` GS stills. Undo restores the
  snapshot and must **not** refit again.

## Preview cache (`PreviewRam gRam`)

- Estimated proxy size `<= 200MB` (`200ull << 20`), or an image: cache the
  **whole** clip, but only when the clip is near the playhead (2s behind,
  14s ahead).
- Larger clips: a window around the playhead (1s behind, 9s ahead). The
  decoded source span is capped at 11s. Evict a cached frame only when it is
  outside `a - 0.15` .. `b + 1.0`, so the playhead does not catch the edge.
- Hard cap stays **1GB** (`1024ull << 20`) plus `RoomFor`: leave at least 2GB
  physical RAM free (or 1/5 of RAM, whichever is larger). Do not raise the cap.
- Solids are not cached. Audio tracks are not cached.
- Once the on-screen clip has about 1.5s cached ahead, the next clip is cached
  before it. A clip that reaches the end of its audio does not seek again.
- The clip that is actually on screen is cached before the one underneath it.
  While playing, no decoder walks more than 4 frames on the UI thread.
- `gRam.Tick` is skipped while export or ASR is running. Export does **not**
  use this cache, and it must not buffer full-resolution frames.
- `PrefetchUpcoming` looks 5s ahead for the next picture change on any channel
  (the clip underneath when this one ends, or a clip that cuts in on top).
  It parks `playerNext` at the **media time of the handoff** (`cutMedia`),
  seeks only when that decoder has no frame or is already past the cut, and
  then walks at most 3 frames per tick. It does **not** skip the decoder just
  because `Covered()` is true. Skip it only when `gRam.Has` has the handoff
  frame. Audio for a clip that starts within 5s is opened then, not on the cut.
- At the cut, `EnsurePlayerForClip` shows the cached frame, or promotes
  `playerUnder` / `playerB` / `playerNext` when that player is already on the
  clip. It does **not** walk hundreds of frames on the UI thread, and it does
  **not** `GrabFrame` a decoder that is still the previous file
  (`playerFileId`). A same-file cut seeks only when the source times do not
  meet. Audio for a clip that starts at the cut is opened before the cut.
- `ExportSession::EnsureFrame` seeks forward only when the decoder is more
  than **1.25s** behind, then walks up to **160** frames. A short sequential
  gap must decode through, not keyframe-seek. A real backward jump still seeks.

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
- `PlacePanel` positions with `ImGuiCond_Appearing` only. Do not use
  `ImGuiCond_Always` and do not call `SetNextWindowFocus` every frame.
  The subtitle style window passes a left shift so it does not cover the
  text window. Title-bar drag and widget selection must keep working.
- BUS has **no "+" button**. Clicking an empty bus lane creates a segment that
  spans the full timeline range, unless more than one object is already
  selected — then the click only unselects (see below).
- The clip right-click **Effects** menu stays gone. Overlay FX on the FX
  lane is separate. Transitions are the tab after Solids, not that menu.
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
`haveB && look.wipe == 0 && bgShowN > 0 && previewFrameValid &&` stencil bits `> 0`.
A wipe uses a scissor on B (`wipe` 1 from the left, 2 from the right, 3 from the top, 4 from the bottom). A dip draws a color quad over the frame after the pair.
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

- Clicking an **already selected** clip must **not** `SelectOnly` (that was
  breaking group drag). Clicking a clip that is not in the selection still
  selects only that one.
- Clicking a different kind clears the others. An FX click (`FocusOverlay`)
  clears clips, subs, bus, and media. A clip, sub, bus, or media click clears
  `gOverlaySel`. Delete follows that selection, **not** `fxHover`. A selected
  effect deletes even if the pointer has left it; a selected clip is not
  deleted just because the pointer is over an effect.
- Trim stays single-item.
- A vertical drag moves the **whole clip selection** by the same channel step
  as the grabbed clip (`FollowLeaderChannel`), and only when that step lands
  on a real channel. Picture ladder, top to bottom: GS, V1, V2, V3. Audio:
  A1, A2, A3. A clip whose next channel does not exist stays. Right-click
  **Channel up** / **Channel down** is one step of the same ladder.
- `SnapTime` / `SnapStart` skip indices in `groupSkip`.
- Clamp the group so nothing starts before 0 (`ApplyGroupDelta`).
- Empty SUBS click **without** a drag still creates a subtitle, but only when
  `UnselectIfMany` does not fire (one selected object, or none).
- `UnselectIfMany`: a click on empty lane space (BUS, FX, GS, SUBS, V/A)
  clears every selection when `SelectedObjectCount() > 1` (clips, subs, media,
  a bus segment, and the selected effect all count). A **single** selection is
  left alone, so a lone click can still start a marquee or create a subtitle.
- Dropping an effect on the FX lane selects it (`OverlayAdd` then
  `FocusOverlay`) and does **not** set `ovPanel` / `ovPanelOpen`. Right-click
  on an existing effect still opens the Overlay panel.
- Delete removes the selected kind. Bus still wins over clips. Media still
  wins over clips. `WantTextInput` blocks delete.
- Right-click menu **Mute**, **Fade In**, **Fade Out**, and **Speed** write
  the clicked clip's new value onto every clip in `selection`. Transform,
  Color, and Audio Effects still open one clip.
- Right-click a GS clip: if it is already in the multi-selection, keep that
  selection; otherwise `SelectOnly`. Open the GS panel and the clip menu.
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

`ExportSession::Begin(out, ew, eh, fps, exRife, exSubMode != 1)`.
`exOutKind` 0 is that video export. 1 is audio only: `BeginMp3` writes the
timeline mix (`MixTimeline`, fades and transition crossfade included) as
MP3, 48 kHz stereo, 192 kbps. No picture and no subtitles. Do not read
`AVCodec::sample_fmts`; this FFmpeg removed it. Try `S16P`, then `FLTP`,
then `S16` with `avcodec_open2`. Do not renumber
`exRes` or `exFpsIdx`.

Phases:

0. Render the timeline into an mp4 at the chosen export size.
1. RIFE (`BeginRifePass` / `StepRife`), only if `exRife` and fps is even and `>= 48`.
   50 and 60 still render at half (`fps/2`) and insert one midpoint (`t = 0.5`).
   72 always turns RIFE on: the movie is rendered at 24, then RIFE inserts two
   frames (`t = 1/3`, `2/3`). That is the ComfyUI multiplier 3 path. `exFpsIdx`
   6 is 72. Do not renumber 0–5.
   120 and 240 always turn RIFE on: the movie is rendered at 60, then RIFE
   inserts one frame for 120 (`t = 0.5`) or three for 240 (`t = 0.25`, `0.5`,
   `0.75`). `exFpsIdx` 0–3 stay 24, 30, 50, 60. 4 is 120, 5 is 240. Do not
   renumber them. The final file is `outFps`. `RifeRun` **refuses** `w` or `h` `> 3840`.
   The UI treats `phase == 1` as the GPU step (max 16 steps / 80ms).

Subtitles: burned during phase 0 only when `burnSubs && !rifeOn`. If RIFE is on,
they burn inside `StepRife`, on the full-rate frames. `StampGlyph` samples the atlas with bilinear alpha and a round stroke. A
sample that leaves that glyph's quad is 0, so the border cannot pick up the
next letter in the atlas. Do not put the eight nearest-neighbor copies back. The preview stroke is the same circle,
and the font texture is `GL_LINEAR`. The subtitle face is Segoe UI at 160px.

`CloseAll` deletes `*.rife-tmp.mp4` and calls `RifeShutdown()` if RIFE was on.
There is no second upscale pass.

8K encoder in the main `Begin` uses `hevc_nvenc` / `libx265` when `w` or `h`
`> 4096`. `BeginRifePass` itself is still h264, and `RifeRun` refuses above 3840.

### RIFE (already shipped)

Windows-only, `onnxruntime.dll` loaded dynamically, model
`models/rife49.onnx` or `rife.onnx` next to the exe (same folder the user
already uses). CUDA via `PreloadCuda` (zlibwapi, CUDA 13, cuDNN). After
cudart loads, `ProbeGpu` calls `cudaGetDeviceCount`, `cudaDeviceGetAttribute`
(compute capability major is attribute 75, minor is 76), `cudaDeviceGetName`,
and `cudaMemGetInfo`. It keeps the NVIDIA GPU with the highest compute
capability (an RTX 5090 is 12.0), then the most VRAM, and `cudaSetDevice`s
that index. ORT gets `device_id`, `gpu_mem_limit` (total minus one eighth,
and not past free minus 512MB), and `use_tf32=1` when the capability is 8
or newer so cuDNN runs the FP32 convolutions on tensor cores. There is no
`.cu` file and no `CMAKE_CUDA_ARCHITECTURES`. Do not add nvcc. If the full
option set is rejected, retry with the memory limit, then with `device_id`
only. The card is written to `rife.log` and to the RIFE status line. Every
`rife.log` line starts with `YYYY-MM-DD HH:MM:SS`. If the CUDA runtime does
not export the device queries, the card is read from `nvcuda.dll` instead.
Each in-between frame is the average of `RifeRun(prev, next, t)` and
`RifeRun(next, prev, 1-t)` (ensemble). Frames are zero-padded to a multiple
of 64, matching RIFE 4.9. `fast_mode` does nothing on rife49. `scale_factor`
1 and float32 are what this ONNX already does. The per-block flow ensemble
inside `rife49.pth` is not in the ONNX graph, so this average is the match
that the file allows. If this `onnxruntime.dll` has no CUDA provider (the
error lists DML and not CUDA), RIFE attaches DirectML and the status says
`RIFE on DirectML`, not `RIFE on GPU`. Do not call that CUDA. Real CUDA still
needs the GPU build's `onnxruntime.dll`, `onnxruntime_providers_shared.dll`,
and `onnxruntime_providers_cuda.dll` next to the exe. Do not
`FreeLibrary` the kept preload module. The build does not
download the DLL or the model. Do not send guessed ONNX Runtime zip URLs.

### Whisper subs (already shipped)

`asr.cpp`. The only models the build copies are `mel80.bin` and
`multilingual.tiktoken`. Do not drop that `POST_BUILD`.

## Edge fades, waveform, timeline zoom

The clip-menu transition list stays gone. `TransitionForClip` composites
only an applied `fxOut` inside the half-open overlap (see decision 15).
Do not turn it on for every overlap, and do not put the menu back.
`effects.cpp` is the transition set. The Effects **tab** (overlay FX) stays.

`Clip` fields: `opIn`, `opOut`, `volIn`, `volOut` (seconds). Saved on a new
`R` line. `audioOpen` is per clip, not saved, default closed. Copied in
`ClipWithoutFx`. The old audio `fadeIn` / `fadeOut` menu items stay.

`GainAt` uses `VolumeEdge` when `volIn` or `volOut` is above 0.02s: 0 at
the outer edge, 1 at the handle, 1 in the body. That multiplies the
envelope. It is **not** `RampAt` (that curve is opacity only). The old
audio `fadeIn` / `fadeOut` menu items stay and apply only when neither
volume edge is set. If this clip is A or B of an active transition, that
gain is then multiplied by the equal-power weight for the overlap.

Opacity (`RampAt` — the user asked to flip the previous curve):

- Neither edge set: 1.
- Left Shift + drag the **start** edge of the **clip body**: 0 at the head
  (bottom of the graph), 100 at the handle, and **100 for the rest of the
  clip** (`opIn`). The clip underneath shows only while the ramp is below 1.
- Left Shift + drag the **end** edge of the body: 100 until the handle, then
  down to 0 at the tail (`opOut`).
- If both are set, the lower ramp wins where they overlap. The body between
  them stays at 100. Do not put the inverted curve back (100 at the outer
  edge, body stuck at 0).
- Volume is **one** yellow line, `envelope * VolumeEdge`, drawn by
  `DrawVolumeLine` on the wave strip. Do not also draw the envelope
  polyline or a second `DrawFadeGraph` for volume. Fade in: the line
  starts at the bottom (silence) and rises to the envelope at `volIn`.
  The straight envelope, including its end point, is hidden until that
  high point (`VolPointHidden`). The high point itself is a real envelope
  point (`EnsureFadeAnchor`). After the fade exists, drag that circle:
  horizontal moves `volIn` / `volOut`, vertical moves the gain the fade
  reaches. The circle is grabbable even where it sits on the boundary
  between the clip body and the wave (`FadeHandleRole` does not require
  the pointer to be inside the wave). That click must not drag the clip.
  Dragging it back to the edge clears the fade. Fade out is the
  same toward the tail. Ctrl + drag an edge of the **open** wave strip
  still sets the fade and moves that same point.
- The wave strip is **per clip** and **closed by default** (`audioOpen`,
  not saved) on V1–V3 and GS. A yellow arrow in the center of the clip
  body toggles it. Click again to close. A1, A2 and A3 do **not** grow.
  The waveform, the yellow volume line, and the volume points sit on the
  clip body itself. There is no arrow and no Shift opacity on those lanes
  (there is no picture). Double-click the body to add a point, drag a
  point to change level, Ctrl + drag an edge for the volume fade. A body
  edge without Ctrl still trims. Do not open every video clip's strip. A
  video or GS lane is the tall height only while some clip on it is open
  (`LaneHasAudioOpen`, `trackLaneH`, `gsChildH`). Marquee height follows
  that clip's strip. An audio-track clip is only the body height.
- Shift on a video or GS body still wins for opacity. It does nothing
  on A1–A3. Ctrl on the body does not fade. Shift on the wave does not
  fade. A body edge without Shift still trims. A click on the open wave
  body drags the clip. The arrow click does not.
- The drag does **not** change the clip bounds. It is not a trim.
- Handle the fade **before** Shift-click `ToggleSelect`, or it never starts.
- State: `fadeDragClip`, `fadeDragSide` (-1 start / +1 end), `fadeDragKind`
  (1 opacity, 2 volume). Clear them in `ClearTimelineDrags`. Length from
  mouse x, clamp `0 .. duration-0.05`. Under ~0.03s clears the fade.
- Draw the white opacity ramp on the clip body. 0 is the bottom, 1 is the
  top. Handle circles sit on the curve. Draw it on every clip that has an
  opacity fade, not only the selected one. On video and GS the selected
  envelope points that are **not** inside a volume fade sit on the wave
  strip, and `dragVol` maps gain across `kWaveH`. On A1–A3 those points
  sit on the clip body, and `dragVol` maps gain across `kBodyH`. Skip
  points `VolPointHidden` hides.
- `OpacityAt` is `RampAt` of `opIn` / `opOut`. `BlitVideo` takes `float cover`.
  Below 0.999 it blends. The grade shader alpha uniform is `cover` (a graded
  clip used to ignore the fade). Ungraded blit sets `GL_MODULATE` so
  `glColor4f` alpha applies. `BlitToCanvas(idx, place, cover)` scales source
  alpha. `cover <= 0.001` draws nothing.
- `FrontLeavesGaps` returns true when the front `OpacityAt` is below 0.98
  and something is underneath, so a faded body shows that clip and not black.
  GS opacity is `gsOpacity * OpacityAt`.

Waveform: white vertical bars, centered. On V1–V3 and GS they are in the
dark strip **under** the clip, and only while that clip's audio strip is
open. On A1–A3 they are drawn on the clip body, and the lane does not
grow. Peaks are per file path, built once on a background thread
(`DecodeWavePeaks`, about 2 buckets/second, 32–2000, packet cap 8000).
`wave` state 2 with an empty vector means no audio — do not retry every
frame. Map through `sourceOffset` and `speed`.

Tracks: body about 52px. The wave (about 36px) is not part of the lane
until a clip on it is open. FX and SUBS stay 42. Do not force every lane
to the open height. The timeline child has `AlwaysVerticalScrollbar` and
its height is capped near half the window, so open channels still scroll.
A master meter sits to the right of the channels (`masterout`), not in
the scroller. The colored bars are the output level (green, then yellow,
then red). The fader under them is `masterGain`, 0 to 1.5, saved as
`master`. It scales `MixTimeline`, so preview and export both follow it.
Do not move that fader onto a video lane.

Zoom: `SliderFloat` `##tlzoom` on the **right**, immediately to the left of
the aspect combo (`##previewaspect`), not beside the transport buttons.
Minimum is a fit of `contentEnd * 1.20` against `tlViewW` (the timeline
child width from the previous frame, minus the label gutter), clamped 1–80.
At that minimum the view is the project plus about 20% empty on the right.
Do not write `pixelsPerSecond` every frame. Default stays 50. Saved as `pps`.
Load accepts 1–400. `tlSeconds` is at least `contentEnd * 1.20`.

## Shipped commits — do not redo

Newest first, all on `origin/main`:

- `ff88e52` had the inverted opacity ramp (100 at the edge, body at 0).
  The user then asked for the opposite. Do not put that inverted ramp back.
- `99f00f2` edge fades, in-clip waveform, zoom fader. The fade curve in that
  commit was the standard one (body stayed at 100). The user rejected it.
- `a110675` compile fix, duplicate `cutMedia`
- `5861d38` channel cut: no decoder walk, audio does not reseek at EOF
- `1aedbd5` cache the visible clip first; playback catch-up capped at 4 frames
- `18eff73` rename `far` (Windows macro)
- `c32ee1c` prefetch the next channel at the handoff media time
- `36c2885` longer cache window, unselect on empty multi-click, FX drop does not open the panel
- `1f5deab` multi-selection channel move, delete the clicked item
- `bbd80f2` GS stills keep their aspect
- `70f53b8` SwinIR removed
- `39424d8` BasicVSR++ checkbox, upscale before RIFE (the option was removed with SwinIR; do not restore it)
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

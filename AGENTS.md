# DaniWorldsStudioDesktop

Windows C++/SDL2/ImGui/OpenGL/FFmpeg editor. This is **not** a web app.
Before any edit, read `.grok/skills/DaniWorldsStudioDesktop/SKILL.md` and follow it.
Do not regress preview handoff, underlay, transition-end snap, group drag,
ripple insert, or panels-above-timeline.

- Local mirror the agent uses: `/tmp/dws`. Remote: `origin` `main`.
- Do not `cmake --build` on Linux. The user builds on Windows with `taskkill`,
  `git pull`, then `cmake --build . --config Release --target DaniWorldsStudioDesktop -- /m:1`.
- Git author and committer: `danimanhillel` / `danimanhillel@users.noreply.github.com`,
  set on the `git commit` command itself. Never `git config`.
- Reply to the user in Hebrew. Do not vendor ONNX weights or `onnxruntime.dll`.
- SwinIR was removed from export. Do not add it back unless the user asks.
  RIFE stays. The `swinir_realsr_x4.onnx` file beside the exe is unused.
- Imported images that enter the GS channel keep their aspect (`ContainGsImage`,
  `gsFit`). Video GS and solids still fill. Do not rebake every frame.
- A multi-selection moves up or down a channel together when that channel
  exists. Mute, fade, and speed from the clip menu apply to every selected
  clip. Clicking an effect clears the clip selection, so Delete removes the
  effect and not a leftover clip.
- A cut between channels must not open a decoder or seek audio on that frame.
  Cache the next clip once the one on screen has ~1.5s ahead. An audio clip
  that reaches its end stays at the end and does not seek again.
- The clip right-click transition menu stays off. Do not bring that menu
  back, and do not restore the inverted opacity ramp. Transitions are the
  tab next to Solids: select two clips that overlap on different video
  channels, then drag the effect onto a video track. `TransitionForClip`
  composites only that applied `fxOut` inside the half-open overlap. The
  incoming clip stays on the main player, so the lower picture does not
  stick after the overlap. Those two clips equal-power crossfade their
  audio for the same span. Left Shift on a clip-body edge fades opacity the
  opposite way from the inverted ramp: left side rises from the bottom to
  the top, right side falls from the top to the bottom, and the body stays
  at 100. Volume is one yellow line: silence at the outer edge, rising to
  the envelope at the handle. The straight envelope is hidden inside that
  fade. The circle at the top of that fade can be dragged afterwards,
  including when it sits on the clip itself: sideways changes the fade
  length, up and down changes its level. Ctrl +
  drag the edge on that clip's audio strip. On V1–V3 and GS the strip is
  closed until the arrow in the middle of the clip; click again to close
  it. A1–A3 do not grow: the waveform, the yellow volume line, and the
  points sit on the clip itself. No Shift there. A master meter and fader
  sit to the right of the channels. The fader is `masterGain` (0 to 1.5)
  and scales the mix that plays and exports. The zoom fader sits on the right, next to the aspect combo. Do not bring the
  transition menu back. Do not restore the inverted opacity ramp.
- Overlay effects stay append-only. Zoom In and Zoom Out are kinds 3 and 4.
  Dust, Cassette, Frame, Film Dust, Retro Dust, Old Film, and Record are
  19–25. Motion Blur is 26, Gradient Blur is 27, and Sharpen is 28
  (`OverlayKindCount` is 29). Do not renumber them. Direction does not rotate
  the blur layers. Sharpen is a strong luminance unsharp mask. Their
  extra knobs are project line `N`. Isotropic Blur / Blur In / Blur Out stay
  5 / 6 / 7. The Media, Effects, Solids,
  and Transitions tabs stay pinned; only the cards under them scroll. Effects cards
  show the bundled plate through the real preview shader, not the flat icon.
  Transition cards play the real effect between the two bundled plates
  (snow then green), ping-pong over 3s. The orange and blue blocks are only
  the fallback. `libtrans` always shows its vertical scrollbar so the cards
  do not jump between two positions. Twenty extra transitions are `ShapeFx`
  shapes 1–20, appended after the original ten and painted by `FxPaint`
  (cards 192×108, preview readback, export `FxComposite`). There is no
  person detector. Do not renumber or drop the original ten.
- Side panels Bus grade, Clip color, Transform, Green screen, Audio Effects,
  Subtitle style, and Overlay have SAVE and LOAD. SAVE is a name only. The
  file lands in `%USERPROFILE%\DaniWorldsStudio\Looks\<kind>\<name>.dlook`,
  not a picked path, and it is not part of the project. Startup scans that
  folder so LOAD is a list of buttons inside the panel. Click a name to load it.
  The label is white and not clipped. Do not put that list in a popup. Each
  effect has its own list (`Looks/fx<kind>/`). Do not share one overlay list
  across effects. Bus and clip color share one file kind. Do not
  put SAVE/LOAD on the clip menu.
- RIFE queries the CUDA runtime for the GPU, or the NVIDIA driver if that
  runtime cannot name the card. A 5090 (compute capability 12.0) wins over a
  weaker card. TF32 is enabled from capability 8 up so cuDNN can use tensor
  cores, and the arena limit comes from VRAM. There is no nvcc architecture
  flag and no `.cu` file. `rife.log` lines start with a date and time. If the
  loaded `onnxruntime.dll` has no CUDA provider, RIFE uses DirectML and the
  status says so. Do not report that as CUDA.
- Burned subtitles sample the font atlas with bilinear coverage and a round
  stroke. A sample outside that glyph is 0, so the border does not pick up
  the next letter. `PlacePanel` uses `ImGuiCond_Appearing` and does not
  refocus every frame, so the subtitle windows can be dragged and edited.
  The style window is shifted left of the text window.
- Export FPS 120 and 240 always use RIFE. The movie is rendered at 60, then
  RIFE inserts one frame (120) or three (240, at 0.25 / 0.5 / 0.75). 72 is
  multiplier 3 from a 24 fps render (timesteps 1/3 and 2/3), `exFpsIdx` 6.
  24, 30, 50 and 60 stay on `exFpsIdx` 0–3. 4 is 120, 5 is 240. Do not
  renumber them. Every RIFE frame is an ensemble average of both directions.
  Export can also write audio only (`exOutKind` 1): MP3 of the timeline mix,
  48 kHz stereo. Video radios stay unchanged.
- The timeline ruler stays pinned while the channels scroll vertically.
  Horizontal scroll stays shared (`tlbody` scroll, ruler follows).
- Preview keeps more seconds ahead (small clips 14s, large clips 9s / 11s
  source). An empty click unselects only when more than one object is
  selected. Dropping an effect does not open its side panel.

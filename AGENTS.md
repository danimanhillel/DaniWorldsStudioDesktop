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
- Clip-menu transitions are off (`TransitionForClip` returns false). The lower
  picture was sticking. Left Shift on a clip-body edge fades opacity the
  opposite way from the inverted ramp: left side rises from the bottom to
  the top, right side falls from the top to the bottom, and the body stays
  at 100. Volume is one yellow line: silence at the outer edge, rising to
  the envelope at the handle. The straight envelope is hidden inside that
  fade. The circle at the top of that fade can be dragged afterwards,
  including when it sits on the clip itself: sideways changes the fade
  length, up and down changes its level. Ctrl +
  drag the edge on that clip's audio strip. The strip is closed
  until the arrow in the middle of the clip; click again to close it. The
  zoom fader sits on the right, next to the aspect combo. Do not bring the
  transition menu back. Do not restore the inverted opacity ramp.
- Overlay effects stay append-only. Zoom In and Zoom Out are kinds 3 and 4.
  Dust, Cassette, Frame, Film Dust, Retro Dust, Old Film, and Record are
  19–25 (shader in preview, `FilmGrade` on export). The Media, Effects, and
  Solids tabs stay pinned; only the cards under them scroll. Effects cards
  show the bundled plate through the real preview shader, not the flat icon.
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
- The timeline ruler stays pinned while the channels scroll vertically.
  Horizontal scroll stays shared (`tlbody` scroll, ruler follows).
- Preview keeps more seconds ahead (small clips 14s, large clips 9s / 11s
  source). An empty click unselects only when more than one object is
  selected. Dropping an effect does not open its side panel.

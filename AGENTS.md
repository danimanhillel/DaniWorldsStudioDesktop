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
  fade. Ctrl + drag the edge on that clip's audio strip. The strip is closed
  until the arrow in the middle of the clip; click again to close it. The
  zoom fader sits on the right, next to the aspect combo. Do not bring the
  transition menu back. Do not restore the inverted opacity ramp.
- Preview keeps more seconds ahead (small clips 14s, large clips 9s / 11s
  source). An empty click unselects only when more than one object is
  selected. Dropping an effect does not open its side panel.

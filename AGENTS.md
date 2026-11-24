# DaniWorldsStudioDesktop

Windows C++/SDL2/ImGui/OpenGL/FFmpeg editor. This is **not** a web app.
Before any edit, read `.grok/skills/daniworlds-studio/SKILL.md` and follow it.
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
- A cut to a clip on another channel uses the cached frame or a decoder that
  was already parked at that media time. Do not seek to the start of the file
  and do not decode hundreds of frames on the cut. While playing, catch up at
  most 4 frames per tick, and cache the visible clip before the one under it.
- Preview keeps more seconds ahead (small clips 14s, large clips 9s / 11s
  source). An empty click unselects only when more than one object is
  selected. Dropping an effect does not open its side panel.

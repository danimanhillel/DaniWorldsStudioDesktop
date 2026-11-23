# DaniWorldsStudioDesktop

Windows C++/SDL2/ImGui/OpenGL/FFmpeg editor. This is **not** a web app.
Before any edit, read `.grok/skills/daniworlds-studio/SKILL.md` and follow it.
Do not regress preview handoff, underlay, transition-end snap, group drag,
ripple insert, panels-above-timeline, or the BasicVSR++ → RIFE export order.

- Local mirror the agent uses: `/tmp/dws`. Remote: `origin` `main`.
- Do not `cmake --build` on Linux. The user builds on Windows with `taskkill`,
  `git pull`, then `cmake --build . --config Release --target DaniWorldsStudioDesktop -- /m:1`.
- Git author and committer: `danimanhillel` / `danimanhillel@users.noreply.github.com`,
  set on the `git commit` command itself. Never `git config`.
- Reply to the user in Hebrew. Do not vendor ONNX weights or `onnxruntime.dll`.
- The build does not download `models\basicvsrpp.onnx`. There is no official
  ONNX of that model; a `.pth` renamed to `.onnx` will not load.

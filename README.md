# DaniWorldsStudio Desktop

Desktop video editor written in C++.

## Features (planned)
- Fullscreen on launch
- GPU accelerated preview (OpenGL)
- Multi-track timeline
- Import video / audio / images
- Easy exit (ESC or window close)

## Build

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

Requires SDL2 and OpenGL.

On Ubuntu/Debian:
```bash
sudo apt install libsdl2-dev
```

## Exit
Press **ESC** or close the window.

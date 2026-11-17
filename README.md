# DaniWorldsStudio Desktop

Desktop video editor written in C++ (SDL2 + OpenGL).

## Features
- Fullscreen on launch
- GPU accelerated (OpenGL)
- Black background
- Exit with **ESC**

## Build (Windows / Linux / macOS)

Requirements:
- CMake 3.16+
- C++ compiler (Visual Studio 2019+ on Windows, or g++/clang)
- Git
- Internet connection (SDL2 is downloaded automatically)

```bash
git clone https://github.com/danimanhillel/DaniWorldsStudioDesktop.git
cd DaniWorldsStudioDesktop
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

### Windows (Visual Studio)
```powershell
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

Then run:
```
.\Release\DaniWorldsStudioDesktop.exe
```

### Windows (MinGW / Ninja)
```powershell
cmake .. -G Ninja
cmake --build .
```

## Exit
Press **ESC** or close the window.

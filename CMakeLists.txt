cmake_minimum_required(VERSION 3.16)
project(DaniWorldsStudioDesktop VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# SDL2
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.8
    GIT_SHALLOW TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

find_package(OpenGL REQUIRED)

# FFmpeg (user must have it installed or use vcpkg)
# On Windows with vcpkg: cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET libavcodec libavformat libavutil libswscale)
endif()

if(NOT FFMPEG_FOUND)
    # Fallback: look for common locations or let user set FFMPEG_DIR
    set(FFMPEG_INCLUDE_DIRS "" CACHE PATH "FFmpeg include directory")
    set(FFMPEG_LIBRARIES "" CACHE STRING "FFmpeg libraries")
    message(STATUS "FFmpeg not found via pkg-config. Set FFMPEG_INCLUDE_DIRS and FFMPEG_LIBRARIES or use vcpkg.")
endif()

add_executable(DaniWorldsStudioDesktop
    src/main.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(DaniWorldsStudioDesktop PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

if(FFMPEG_FOUND)
    target_include_directories(DaniWorldsStudioDesktop PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(DaniWorldsStudioDesktop PRIVATE ${FFMPEG_LIBRARIES})
    target_compile_definitions(DaniWorldsStudioDesktop PRIVATE HAVE_FFMPEG)
else()
    message(WARNING "FFmpeg not linked. Video preview will be limited.")
endif()

target_link_libraries(DaniWorldsStudioDesktop
    PRIVATE
    SDL2::SDL2-static
    OpenGL::GL
)

if(WIN32)
    target_link_libraries(DaniWorldsStudioDesktop PRIVATE SDL2::SDL2main)
    set_target_properties(DaniWorldsStudioDesktop PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

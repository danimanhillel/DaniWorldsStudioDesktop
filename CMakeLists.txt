cmake_minimum_required(VERSION 3.16)
project(DaniWorldsStudioDesktop VERSION 0.4.0 LANGUAGES CXX)

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

# FFmpeg - prefer user-provided paths
set(FFMPEG_INCLUDE_DIRS "" CACHE PATH "FFmpeg include directory")
set(FFMPEG_LIBRARIES "" CACHE STRING "FFmpeg libraries (semicolon-separated)")

set(HAVE_FFMPEG FALSE)
if(FFMPEG_INCLUDE_DIRS AND FFMPEG_LIBRARIES)
    set(HAVE_FFMPEG TRUE)
    message(STATUS "Using user-provided FFmpeg")
    get_filename_component(FFMPEG_ROOT "${FFMPEG_INCLUDE_DIRS}" DIRECTORY)
    if(EXISTS "${FFMPEG_ROOT}/lib/swresample.lib")
        list(APPEND FFMPEG_LIBRARIES "${FFMPEG_ROOT}/lib/swresample.lib")
        message(STATUS "Adding swresample: ${FFMPEG_ROOT}/lib/swresample.lib")
    endif()
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET libavcodec libavformat libavutil libswscale libswresample)
        if(FFMPEG_FOUND)
            set(HAVE_FFMPEG TRUE)
        endif()
    endif()
endif()

add_executable(DaniWorldsStudioDesktop
    src/main.cpp
    src/effects.cpp
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

if(HAVE_FFMPEG)
    target_include_directories(DaniWorldsStudioDesktop PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(DaniWorldsStudioDesktop PRIVATE ${FFMPEG_LIBRARIES})
    target_compile_definitions(DaniWorldsStudioDesktop PRIVATE HAVE_FFMPEG)
    message(STATUS "FFmpeg enabled")
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

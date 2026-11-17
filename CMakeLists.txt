cmake_minimum_required(VERSION 3.16)
project(DaniWorldsStudioDesktop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Download SDL2 automatically (works on Windows, Linux, macOS)
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.8
    GIT_SHALLOW TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

find_package(OpenGL REQUIRED)

add_executable(DaniWorldsStudioDesktop
    src/main.cpp
)

target_link_libraries(DaniWorldsStudioDesktop
    PRIVATE
    SDL2::SDL2-static
    OpenGL::GL
)

if(WIN32)
    target_link_libraries(DaniWorldsStudioDesktop PRIVATE SDL2::SDL2main)
    # Hide console window on Windows
    set_target_properties(DaniWorldsStudioDesktop PROPERTIES
        WIN32_EXECUTABLE TRUE
    )
endif()

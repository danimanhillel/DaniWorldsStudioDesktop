cmake_minimum_required(VERSION 3.16)
project(DaniWorldsStudioDesktop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(DaniWorldsStudioDesktop
    src/main.cpp
)

target_link_libraries(DaniWorldsStudioDesktop
    PRIVATE
    SDL2::SDL2
    OpenGL::GL
)

if(WIN32)
    target_link_libraries(DaniWorldsStudioDesktop PRIVATE SDL2::SDL2main)
endif()

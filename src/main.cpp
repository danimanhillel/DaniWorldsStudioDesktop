#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <iostream>

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* window = SDL_CreateWindow(
        "DaniWorldsStudio Desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1920, 1080,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window) {
        std::cerr << "Window creation failed\n";
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("DaniWorldsStudio", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::Text("DaniWorldsStudio Desktop");
        ImGui::SameLine();
        if (ImGui::Button("Import")) {}
        ImGui::SameLine();
        if (ImGui::Button("Export")) {}
        ImGui::SameLine();
        ImGui::Text("| ESC = Exit");

        ImGui::Separator();

        float topHeight = io.DisplaySize.y * 0.42f;
        ImGui::BeginChild("MediaPanel", ImVec2(io.DisplaySize.x * 0.28f, topHeight), true);
        ImGui::Text("Media Library");
        ImGui::Separator();
        ImGui::Text("Import media here...");
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("PreviewPanel", ImVec2(0, topHeight), true);
        ImGui::Text("Preview");
        ImGui::Separator();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("preview_area", ImVec2(avail.x, avail.y - 45));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetItemRectMin();
        ImVec2 p1 = ImGui::GetItemRectMax();
        draw->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
        draw->AddText(ImVec2(p0.x + 12, p0.y + 12), IM_COL32(200, 200, 200, 255), "Preview");

        if (ImGui::Button("Play")) {}
        ImGui::SameLine();
        if (ImGui::Button("Pause")) {}
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {}
        ImGui::EndChild();

        ImGui::BeginChild("Timeline", ImVec2(0, 0), true);
        ImGui::Text("Timeline");
        ImGui::Separator();
        ImGui::Text("Video tracks: V1  V2  V3");
        ImGui::Text("Audio tracks: A1  A2  A3");
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.04f, 0.04f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

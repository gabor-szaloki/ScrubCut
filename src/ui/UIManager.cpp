#include "ui/UIManager.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_opengl3.h>
#include "util/AppPaths.h"

bool UIManager::Init(SDL_Window* window, SDL_GLContext glContext) {
    (void)glContext;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    m_iniPath = (GetAppDataDir() / "imgui.ini").string();
    io.IniFilename = m_iniPath.c_str();

    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForOpenGL(window, glContext))
        return false;
    if (!ImGui_ImplOpenGL3_Init("#version 150"))
        return false;

    return true;
}

void UIManager::Shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void UIManager::BeginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    SetupDockspace();
}

void UIManager::EndFrame() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void UIManager::SetupDockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpace", nullptr, flags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspaceId = ImGui::GetID("MainDockspace");

    // Build default layout on first run (no saved layout yet)
    if (!m_layoutInitialized) {
        m_layoutInitialized = true;

        if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

            // Split: top ~84% / bottom ~16% (compact timeline)
            ImGuiID topId, bottomId;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Down, 0.16f, &bottomId, &topId);

            // Split top: left 75% (viewport) / right 25% (segments)
            ImGuiID viewportId, segmentsId;
            ImGui::DockBuilderSplitNode(topId, ImGuiDir_Right, 0.25f, &segmentsId, &viewportId);

            ImGui::DockBuilderDockWindow("Viewport", viewportId);
            ImGui::DockBuilderDockWindow("Segments", segmentsId);
            ImGui::DockBuilderDockWindow("Timeline", bottomId);

            ImGui::DockBuilderFinish(dockspaceId);
        }
    }

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();
}

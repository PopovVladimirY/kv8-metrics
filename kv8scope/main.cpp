// kv8scope -- Kv8 Software Oscilloscope
// main.cpp -- GLFW/ImGui/ImPlot bootstrap, --config parsing, render loop.

#include "App.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void GlfwErrorCallback(int error, const char* description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static std::string ParseConfigPath(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (strcmp(argv[i], "--config") == 0)
        {
            return argv[i + 1];
        }
    }
    return "kv8scope.json";  // default
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    std::string sConfigPath = ParseConfigPath(argc, argv);
    printf("kv8scope: config = %s\n", sConfigPath.c_str());

    // ---- GLFW init --------------------------------------------------------
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit())
    {
        fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    // OpenGL 3.3 core
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* pWindow = glfwCreateWindow(1600, 900, "kv8scope",
                                           nullptr, nullptr);
    if (!pWindow)
    {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(pWindow);
    glfwSwapInterval(0);  // disable vsync -- frame pacing done by software limiter

    // ---- Dear ImGui init --------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(pWindow, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ---- ImPlot init ------------------------------------------------------
    ImPlot::CreateContext();

    // ---- Application ------------------------------------------------------
    App app(sConfigPath);
    app.BuildInitialFonts();

    // ---- Frame rate limiter -----------------------------------------------
    // 30 fps is sufficient for data visualisation and reduces CPU/GPU pressure,
    // leaving more headroom for the consumer threads.
    using FrameClock = std::chrono::steady_clock;
    constexpr std::chrono::microseconds kFrameBudget(33333); // ~30 fps
    auto tpFrameStart = FrameClock::now();

    // ---- Main loop --------------------------------------------------------
    while (!glfwWindowShouldClose(pWindow) && !app.WantsExit())
    {
        tpFrameStart = FrameClock::now();
        glfwPollEvents();

        // Font atlas rebuild (triggered by View > Font menu selection).
        // This version of the imgui docking backend (June 2025+) uses
        // ImGuiBackendFlags_RendererHasTextures: io.Fonts->Clear()+Build()
        // queues the atlas as WantCreate; ImGui_ImplOpenGL3_NewFrame()
        // processes the queue and uploads the new texture automatically.
        // DestroyDeviceObjects/CreateDeviceObjects must NOT be called here
        // (they tear down shaders and VBOs which are unrelated to fonts).
        if (app.FontsNeedRebuild())
            app.DoFontRebuild();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.Render();

        ImGui::Render();

        int iDisplayW = 0;
        int iDisplayH = 0;
        glfwGetFramebufferSize(pWindow, &iDisplayW, &iDisplayH);
        glViewport(0, 0, iDisplayW, iDisplayH);
        glClearColor(0.051f, 0.067f, 0.090f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(pWindow);

        // ---- Frame pacing: sleep until the 33.3 ms budget is spent ------
        auto tpFrameEnd  = FrameClock::now();
        auto elapsed     = tpFrameEnd - tpFrameStart;
        if (elapsed < kFrameBudget)
            std::this_thread::sleep_for(kFrameBudget - elapsed);
    }

    // ---- Cleanup ----------------------------------------------------------
    // App destructor runs before ImGui/GLFW teardown (RAII order).
    // Explicit destructor scope to ensure App is gone before contexts.
    {
        // app destructor called here at scope exit
    }

    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(pWindow);
    glfwTerminate();
    return 0;
}

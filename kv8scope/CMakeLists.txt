cmake_minimum_required(VERSION 3.20)
project(kv8scope LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── GLFW ──────────────────────────────────────────────────────────────────────
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_WAYLAND  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)

# ── Dear ImGui (docking branch) ──────────────────────────────────────────────
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  TRUE
)

# ── ImPlot ───────────────────────────────────────────────────────────────────
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
    DOWNLOAD_ONLY  TRUE
)

# ── nlohmann/json ────────────────────────────────────────────────────────────
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# ── Fetch everything ─────────────────────────────────────────────────────────
FetchContent_MakeAvailable(glfw nlohmann_json imgui implot)

# ── OpenGL ───────────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

# ── Build Dear ImGui as a static library ─────────────────────────────────────
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    # Backends
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui PUBLIC
    glfw
    OpenGL::GL
)

# ── Build ImPlot as a static library ─────────────────────────────────────────
add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp
)

target_include_directories(implot PUBLIC
    ${implot_SOURCE_DIR}
)

target_link_libraries(implot PUBLIC imgui)

# ── kv8scope executable ─────────────────────────────────────────────────────
set(KV8SCOPE_SOURCES
    main.cpp
    App.h
    App.cpp
    AnnotationStore.h
    AnnotationStore.cpp
    ConfigStore.h
    ConfigStore.cpp
    ConnectionDialog.h
    ConnectionDialog.cpp
    ConsumerThread.h
    ConsumerThread.cpp
    CounterTree.h
    CounterTree.cpp
    ScopeWindow.h
    ScopeWindow.cpp
    SessionListPanel.h
    SessionListPanel.cpp
    SessionManager.h
    SessionManager.cpp
    SpscRingBuffer.h
    StatsEngine.h
    StatsEngine.cpp
    StatsPanel.h
    StatsPanel.cpp
    LogStore.h
    LogStore.cpp
    LogPanel.h
    LogPanel.cpp
    FontManager.h
    FontManager.cpp
    ThemeManager.h
    ThemeManager.cpp
    TimeConverter.h
    TimeConverter.cpp
    WaveformRenderer.h
    WaveformRenderer.cpp
)

add_executable(kv8scope ${KV8SCOPE_SOURCES})

target_compile_definitions(kv8scope PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(kv8scope PRIVATE
    kv8util
    imgui
    implot
    nlohmann_json::nlohmann_json
    $<$<PLATFORM_ID:Windows>:comdlg32>
)

set_target_properties(kv8scope PROPERTIES
    VS_GLOBAL_ResolveAssemblyWarnOrErrorOnTargetArchitectureMismatch "None"
)

install(TARGETS kv8scope RUNTIME DESTINATION bin)

cmake_minimum_required(VERSION 3.20)
project(VarMeControl)

set(CMAKE_CXX_STANDARD 17)

# --- THE FIX: Force Static Runtime (/MTd) ---
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI_SOURCES
        lib/imgui/imgui.cpp
        lib/imgui/imgui_draw.cpp
        lib/imgui/imgui_widgets.cpp
        lib/imgui/imgui_tables.cpp
        lib/imgui/backends/imgui_impl_win32.cpp
        lib/imgui/backends/imgui_impl_dx11.cpp
)

# --- FORCE STATIC RUNTIME (/MTd for Debug, /MT for Release) ---
if(MSVC)
    set(variables
            CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG
            CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE
    )
    foreach(variable ${variables})
        if(${variable} MATCHES "/MD")
            string(REPLACE "/MD" "/MT" ${variable} "${${variable}}")
        endif()
    endforeach()
endif()

# 1. Headers
include_directories(include)
include_directories(lib/ViGEmClient/include)

# 2. Executable
add_executable(VarMeControl WIN32 main.cpp app_icon.rc ${IMGUI_SOURCES})

if(MSVC)
    target_link_options(VarMeControl PRIVATE "/ENTRY:mainCRTStartup")
endif()

# Include ImGui folder
target_include_directories(VarMeControl PRIVATE lib/imgui)

# Link DirectX 11
target_link_libraries(VarMeControl PRIVATE d3d11.lib d3dcompiler.lib)
# ... (Keep your headers and add_executable lines as they were)

# 3. Linking with conflict resolution
target_link_libraries(VarMeControl PRIVATE
        "${CMAKE_SOURCE_DIR}/lib/hidapi.lib"
        "${CMAKE_SOURCE_DIR}/lib/ViGEmClient.lib"
        setupapi.lib
        gdiplus.lib
)

# Tell the linker to ignore the Dynamic (DLL) versions of the runtime
# to stop the "already defined" LNK2005 errors.
set_target_properties(VarMeControl PROPERTIES LINK_FLAGS
        "/NODEFAULTLIB:MSVCRTD /NODEFAULTLIB:ucrtd"
)

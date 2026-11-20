cmake_minimum_required(VERSION 3.20)
project(Wolfe VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- third-party code, built from the copies in third_party -----------------

# RNNoise: noise suppression for voice.
add_library(rnnoise STATIC
    third_party/rnnoise/src/celt_lpc.c
    third_party/rnnoise/src/denoise.c
    third_party/rnnoise/src/kiss_fft.c
    third_party/rnnoise/src/pitch.c
    third_party/rnnoise/src/rnn.c
    third_party/rnnoise/src/rnn_data.c
    third_party/rnnoise/src/rnn_reader.c)
target_include_directories(rnnoise PUBLIC third_party/rnnoise/include)
target_compile_options(rnnoise PRIVATE -w)

# Dear ImGui (drawing through Direct3D 11) and imnodes (the node canvas).
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imnodes/imnodes.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imnodes)
target_compile_options(imgui PRIVATE -w)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi gdi32 imm32)

# --- Wolfe -------------------------------------------------------------------

# The audio engine: devices, applications, effects, drift compensation.
add_library(wolfe_engine STATIC
    src/common.cpp
    src/config.cpp
    src/devices.cpp
    src/effect.cpp
    src/graph.cpp
    src/input.cpp
    src/output.cpp
    src/process_finder.cpp
    src/source_buffer.cpp
    src/source_reader.cpp
    src/usbip_server.cpp
    src/virtual_devices.cpp)
target_include_directories(wolfe_engine PUBLIC src third_party)
target_compile_options(wolfe_engine PRIVATE -Wall -Wextra)
target_link_libraries(wolfe_engine PUBLIC rnnoise ole32 mmdevapi avrt ws2_32 cfgmgr32 version)

# What the graphical editor does, without the drawing.
add_library(wolfe_app STATIC src/app/controller.cpp)
target_compile_options(wolfe_app PRIVATE -Wall -Wextra)
target_link_libraries(wolfe_app PUBLIC wolfe_engine)

# The program: the patch canvas in a window, with an icon in the notification area.
add_executable(wolfe WIN32
    src/gui/main.cpp
    src/gui/editor.cpp
    src/gui/selftest.cpp
    src/gui/shell.cpp)
target_compile_options(wolfe PRIVATE -Wall -Wextra)
# Windows' own constants (cursors and the like) in their wide-character form.
target_compile_definitions(wolfe PRIVATE UNICODE _UNICODE)
target_link_libraries(wolfe PRIVATE wolfe_app imgui shell32)
# -static: no MinGW runtime DLLs needed next to the exe.
target_link_options(wolfe PRIVATE -static)

# The same engine from a console, running a graph file.
add_executable(wolfe-cli src/cli/main.cpp)
target_compile_options(wolfe-cli PRIVATE -Wall -Wextra)
target_link_libraries(wolfe-cli PRIVATE wolfe_engine shell32)
target_link_options(wolfe-cli PRIVATE -static)

# --- tools and tests ---------------------------------------------------------

# Test signal player and recorder, used to measure the engine on real devices.
add_executable(wolfe-sigtool tools/sigtool.cpp)
target_compile_options(wolfe-sigtool PRIVATE -Wall -Wextra)
target_link_libraries(wolfe-sigtool PRIVATE wolfe_engine)
target_link_options(wolfe-sigtool PRIVATE -static)

# Prototype: a USB audio device served over USB/IP, for usbip-win2 to attach.
add_executable(wolfe-uacproto tools/uacproto.cpp)
target_compile_options(wolfe-uacproto PRIVATE -Wall -Wextra)
target_link_libraries(wolfe-uacproto PRIVATE ws2_32 avrt winmm)
target_link_options(wolfe-uacproto PRIVATE -static)

# Checks of the drift compensation that run without any audio device.
add_executable(wolfe-enginetest tests/engine_test.cpp)
target_compile_options(wolfe-enginetest PRIVATE -Wall -Wextra)
target_link_libraries(wolfe-enginetest PRIVATE wolfe_engine)
target_link_options(wolfe-enginetest PRIVATE -static)

# Checks of the editor's logic, with made-up devices.
add_executable(wolfe-controllertest tests/controller_test.cpp)
target_compile_options(wolfe-controllertest PRIVATE -Wall -Wextra)
target_link_libraries(wolfe-controllertest PRIVATE wolfe_app)
target_link_options(wolfe-controllertest PRIVATE -static)

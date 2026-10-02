cmake_minimum_required(VERSION 3.20)
project(DLSS5NR01 C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS)

set(NGX_DIR ${CMAKE_SOURCE_DIR}/external/ngx)
set(NGX_LIB ${NGX_DIR}/lib/nvsdk_ngx_s.lib)
if(NOT EXISTS ${NGX_LIB} OR NOT EXISTS ${NGX_DIR}/include/nvsdk_ngx.h)
    message(FATAL_ERROR "NVIDIA DLSS SDK not found. Put its headers in external/ngx/include and nvsdk_ngx_s.lib in external/ngx/lib (see external/ngx/README.md). It is NVIDIA's, under NVIDIA's licence, so it is not part of this repository: tools/fetch_ngx_sdk.ps1 fetches it.")
endif()

# ---- third party (fetched at configure time) ----
include(FetchContent)
# ImGui docking branch. Must match the layout compiled into the installed LS Addon Manager (Lossless.dll).
# Re-pinned 2026-09-10 to the commit our own from-source manager build resolved docking HEAD to.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        367b2c24f399988ddafc0bb4628da0106bcc09be)
FetchContent_MakeAvailable(imgui)   # Dear ImGui has no CMake project of its own: this only fetches it

# ---- forwarder: the only module allowed to call the DLSSNR snippet directly ----
add_library(nr_forwarder SHARED src/forwarder/nr_forwarder.cpp)
target_include_directories(nr_forwarder PRIVATE src)
target_link_libraries(nr_forwarder PRIVATE d3d12)
set_target_properties(nr_forwarder PROPERTIES OUTPUT_NAME "nvngx.dll_dlss5nr01" PREFIX "" SUFFIX ".dll")

# ---- the compatibility self-test: does this model file work on this graphics card? Started by the addon in its own process ----
add_executable(nr_selftest src/selftest/nr_selftest.cpp)
target_include_directories(nr_selftest PRIVATE src ${NGX_DIR}/include)
target_link_libraries(nr_selftest PRIVATE ${NGX_LIB} d3d12 dxgi user32 advapi32 shlwapi ole32 version bcrypt)
# the addon's version, for the shareable report (--report): from addon.json, so it is never out of step
file(READ ${CMAKE_SOURCE_DIR}/addon.json NR_ADDON_JSON)
string(REGEX MATCH "\"version\": *\"([0-9.]+)\"" NR_ADDON_JSON_MATCH "${NR_ADDON_JSON}")
target_compile_definitions(nr_selftest PRIVATE NR_ADDON_VERSION_TEXT="${CMAKE_MATCH_1}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/addon.json)
add_dependencies(nr_selftest nr_forwarder)

# ---- the addons: DLSS 5 Neural Rendering (DLSS5NR01), the DLSS Upscaler (DLSS4DLAA), the FSR Upscaler (FSR3UPSC) and the XeSS Upscaler (XESSUPSC),
# from the same sources (src/addon/product.h) ----
# Intel's XeSS SDK (headers and libxess.dll, not redistributed in source form): tools/fetch_xess_sdk.ps1 puts it in external/xess; without it
# the XeSS Upscaler is not built and the others do not need it (libxess.dll is loaded by its full path at run time)
set(XESS_DIR ${CMAKE_SOURCE_DIR}/external/xess)
set(NR_ADDON_SOURCES
    src/addon/addon.cpp src/addon/runtime.cpp src/addon/panel.cpp src/addon/tasks.cpp src/addon/settings.cpp src/addon/log.cpp
    src/addon/screenshot.cpp src/addon/screenshot_pixels.cpp src/addon/hud_editor.cpp src/addon/auto_quality.cpp src/addon/gpu_load.cpp
    src/addon/bridge.cpp src/addon/frame_tap.cpp src/addon/gpu_timer11.cpp src/addon/frame_trace.cpp src/addon/diagnosis.cpp
    src/addon/present_hook.cpp src/addon/compose11.cpp src/addon/requirements.cpp src/addon/scaler11.cpp src/addon/framegen11.cpp src/addon/hdr.cpp src/addon/lsrec.cpp src/addon/recorder.cpp
    src/engine/nr_engine.cpp
    src/engine/dlaa_model.cpp src/engine/sr_engine.cpp src/engine/flow_estimator.cpp src/engine/fg_engine.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
function(nr_addon target)
    add_library(${target} SHARED ${NR_ADDON_SOURCES})
    target_include_directories(${target} PRIVATE src ${NGX_DIR}/include ${CMAKE_SOURCE_DIR}/third_party/ffx/include ${XESS_DIR}/inc ${CMAKE_SOURCE_DIR}/../../manager/sdk/include ${imgui_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE ${NGX_LIB} d3d12 d3d11 dxgi d3dcompiler user32 gdi32 advapi32 shlwapi ole32 shell32 comdlg32 windowscodecs bcrypt)
    set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "${target}")
    # Keep a PDB and a linker map next to the Release DLL so the offsets in a crash backtrace can be resolved.
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF /MAP>)
    endif()
endfunction()

nr_addon(DLSS5NR01)
add_dependencies(DLSS5NR01 nr_forwarder)

nr_addon(DLSS4DLAA)
target_compile_definitions(DLSS4DLAA PRIVATE NR_PRODUCT_DLAA)
# NVIDIA's DLSS runtime, in a dlss folder next to the built addon (as in the release), where the DLAA addon points NGX
if(EXISTS ${NGX_DIR}/bin/nvngx_dlss.dll)
    add_custom_command(TARGET DLSS4DLAA POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:DLSS4DLAA>/dlss
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${NGX_DIR}/bin/nvngx_dlss.dll $<TARGET_FILE_DIR:DLSS4DLAA>/dlss)
endif()

nr_addon(FSR3UPSC)
target_compile_definitions(FSR3UPSC PRIVATE NR_PRODUCT_FSR)
# frame generation of our own, a prototype: never in a release until it is ready (-DNR_FRAMEGEN=ON for a test build)
option(NR_FRAMEGEN "Frame generation of our own in the FSR Upscaler (a prototype)" OFF)
if(NR_FRAMEGEN)
    target_compile_definitions(FSR3UPSC PRIVATE NR_FRAMEGEN=1)
endif()
# AMD's FidelityFX runtime (FSR 3.1), in an fsr folder next to the built addon (as in the release): tools/fetch_ffx_sdk.ps1 fetches it
set(FFX_DLL ${CMAKE_SOURCE_DIR}/external/ffx/bin/amd_fidelityfx_dx12.dll)
if(EXISTS ${FFX_DLL})
    add_custom_command(TARGET FSR3UPSC POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:FSR3UPSC>/fsr
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFX_DLL} $<TARGET_FILE_DIR:FSR3UPSC>/fsr)
endif()

if(EXISTS ${XESS_DIR}/inc/xess/xess_d3d12.h)
    nr_addon(XESSUPSC)
    target_compile_definitions(XESSUPSC PRIVATE NR_PRODUCT_XESS)
    # Intel's XeSS runtime, in an xess folder next to the built addon (as in the release)
    if(EXISTS ${XESS_DIR}/bin/libxess.dll)
        add_custom_command(TARGET XESSUPSC POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:XESSUPSC>/xess
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XESS_DIR}/bin/libxess.dll $<TARGET_FILE_DIR:XESSUPSC>/xess)
    endif()
else()
    message(STATUS "Intel's XeSS SDK is not in external/xess (tools/fetch_xess_sdk.ps1): the XeSS Upscaler is not built")
endif()

# ---- offline test host: loads the built addon with a fake IHost, headless ImGui and a synthetic LSFG pattern ----
add_executable(nr_hosttest tools/addon_host_test.cpp src/addon/lsrec.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(nr_hosttest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/../../manager/sdk/include ${CMAKE_SOURCE_DIR}/tools ${CMAKE_SOURCE_DIR}/../../manager/tools ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(nr_hosttest PRIVATE d3d11 dxgi d3dcompiler)
add_dependencies(nr_hosttest DLSS5NR01 DLSS4DLAA FSR3UPSC)
if(TARGET XESSUPSC)
    add_dependencies(nr_hosttest XESSUPSC)
endif()

# nr_fgeval: frame generation scored offline on a recording (tools/fg_eval.cpp): every other frame rebuilt by AMD's FSR 3.1 or Intel's
# XeSS frame generation, fed this project's motion estimate, and compared with the real one
add_executable(nr_fgeval tools/fg_eval.cpp src/addon/lsrec.cpp src/engine/flow_estimator.cpp src/engine/fg_engine.cpp)
target_include_directories(nr_fgeval PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party/ffx/include ${XESS_DIR}/inc)
if(EXISTS ${XESS_DIR}/bin/libxess_fg.dll)   # Intel's frame generation runtime, for gen=xess
    add_custom_command(TARGET nr_fgeval POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:nr_fgeval>/xess
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${XESS_DIR}/bin/libxess_fg.dll ${XESS_DIR}/bin/libxell.dll $<TARGET_FILE_DIR:nr_fgeval>/xess)
endif()
target_link_libraries(nr_fgeval PRIVATE d3d12 dxgi d3dcompiler shell32)
if(MSVC)
    target_compile_options(nr_fgeval PRIVATE /openmp)   # the CPU motion refinement (RefineMotion)
endif()
add_dependencies(nr_fgeval FSR3UPSC)   # (AMD's runtime, in the build's fsr folder)

# nr_sreval: an upscaler scored offline on a recording (tools/sr_eval.cpp): each frame shrunk and upscaled back by the addon's own engine
add_executable(nr_sreval tools/sr_eval.cpp src/addon/lsrec.cpp src/engine/sr_engine.cpp src/engine/flow_estimator.cpp)
target_include_directories(nr_sreval PRIVATE ${CMAKE_SOURCE_DIR}/src ${NGX_DIR}/include ${CMAKE_SOURCE_DIR}/third_party/ffx/include ${XESS_DIR}/inc)
target_link_libraries(nr_sreval PRIVATE ${NGX_LIB} d3d12 dxgi d3dcompiler shell32)
add_dependencies(nr_sreval FSR3UPSC)   # (AMD's runtime, in the build's fsr folder)

# nr_composebench: the GPU time of Neural Rendering's compose pass, run back to back on a made-up 4K frame (tools/compose_bench.cpp)
add_executable(nr_composebench tools/compose_bench.cpp src/addon/compose11.cpp)
target_include_directories(nr_composebench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nr_composebench PRIVATE d3d11 dxgi d3dcompiler)

# nr_xadapter: the plumbing for running the model on a second graphics card (a cross-adapter buffer and fence, tools/xadapter_test.cpp; docs/dual-gpu.md)
add_executable(nr_xadapter tools/xadapter_test.cpp)
target_link_libraries(nr_xadapter PRIVATE d3d12 dxgi)

# ---- Neural Rendering offline: runs the model on a recording and scores the flicker it adds (needs the user's nvngx_dlssnr.dll) ----
add_executable(nr_nreval tools/nr_eval.cpp src/addon/lsrec.cpp src/engine/nr_engine.cpp src/engine/dlaa_model.cpp src/engine/flow_estimator.cpp)
target_include_directories(nr_nreval PRIVATE ${CMAKE_SOURCE_DIR}/src ${NGX_DIR}/include)
target_link_libraries(nr_nreval PRIVATE ${NGX_LIB} d3d12 dxgi d3dcompiler shell32)
add_dependencies(nr_nreval nr_forwarder)

# ---- offline test of the frame tap (which pass is the capture, the motion hand-over, presents); a D3D11 device, no model, no SDK at run time ----
add_executable(nr_diagtest tools/diag_test.cpp src/addon/diagnosis.cpp)   # the "what is wrong" rules
target_include_directories(nr_diagtest PRIVATE src)
add_executable(nr_tracetest tools/trace_test.cpp src/addon/frame_trace.cpp)   # the frame trace's ring and export
target_include_directories(nr_tracetest PRIVATE src)
add_executable(nr_taptest tools/tap_test.cpp src/addon/frame_tap.cpp)
target_include_directories(nr_taptest PRIVATE src)
target_link_libraries(nr_taptest PRIVATE d3d11)

# ---- offline test of the settings and looks (the table, looks as text and back, the settings file through a stand-in host)
add_executable(nr_rectest tools/record_test.cpp src/addon/lsrec.cpp)
target_include_directories(nr_rectest PRIVATE src)
add_executable(nr_lsrec tools/lsrec_tool.cpp src/addon/lsrec.cpp src/addon/screenshot_pixels.cpp)
target_include_directories(nr_lsrec PRIVATE src)
add_executable(nr_settingstest tools/settings_test.cpp src/addon/settings.cpp src/addon/screenshot_pixels.cpp)
target_include_directories(nr_settingstest PRIVATE src ${CMAKE_SOURCE_DIR}/../../manager/sdk/include)

# ---- offline test of auto quality (a simulated model)
# nr_gpuloadtest: the card-load sensor of the "Keep the graphics card under a limit" setting, read a few times (tools/gpu_load_test.cpp)
add_executable(nr_gpuloadtest tools/gpu_load_test.cpp src/addon/gpu_load.cpp)
target_include_directories(nr_gpuloadtest PRIVATE src)
add_executable(nr_autotest tools/auto_test.cpp src/addon/auto_quality.cpp)
target_include_directories(nr_autotest PRIVATE src)

# ---- offline test of the requirements checker (no model, no NVIDIA SDK needed at run time) ----
add_executable(nr_reqtest tools/requirements_test.cpp src/addon/requirements.cpp)
target_include_directories(nr_reqtest PRIVATE src)
target_link_libraries(nr_reqtest PRIVATE dxgi bcrypt)

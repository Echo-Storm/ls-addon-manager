cmake_minimum_required(VERSION 3.20)
project(flowlab CXX)
# A lab, standalone: NOT part of the manager's or the addons' build, not run by CI.
# It needs NVIDIA's Optical Flow API headers (MIT-licensed, four files; see addons/DLSS5NR01/external/nvof/README.md) and the driver's nvofapi64.dll.
set(NVOF "${CMAKE_CURRENT_SOURCE_DIR}/../../addons/DLSS5NR01/external/nvof/include" CACHE PATH "folder with nvOpticalFlow*.h")
if(NOT EXISTS "${NVOF}/nvOpticalFlowD3D11.h")
  message(FATAL_ERROR "nvOpticalFlow headers not found at ${NVOF}")
endif()
set(CMAKE_CXX_STANDARD 17)
set(NR_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../addons/DLSS5NR01/src")
add_executable(flowlab flowlab.cpp "${NR_SRC}/addon/lsrec.cpp" "${NR_SRC}/engine/flow_estimator.cpp")
target_include_directories(flowlab PRIVATE "${NVOF}" "${NR_SRC}")
target_link_libraries(flowlab PRIVATE d3d11 d3d12 dxgi d3dcompiler)
target_compile_definitions(flowlab PRIVATE _CRT_SECURE_NO_WARNINGS)

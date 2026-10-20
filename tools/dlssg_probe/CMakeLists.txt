cmake_minimum_required(VERSION 3.20)
project(dlssg_probe CXX)
# Standalone: NOT part of the manager's or the addons' build, and not run by CI. It needs NVIDIA's Streamline SDK, which is not in the repository:
#   addons/DLSS5NR01/external/streamline/sdk   (v2.14.16 from github.com/NVIDIA-RTX/Streamline; ignored by git)
set(SL_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../addons/DLSS5NR01/external/streamline/sdk" CACHE PATH "Streamline SDK folder")
if(NOT EXISTS "${SL_SDK}/include/sl.h")
  message(FATAL_ERROR "Streamline SDK not found at ${SL_SDK} (see the comment at the top of this file)")
endif()
set(CMAKE_CXX_STANDARD 17)
add_executable(dlssg_probe dlssg_probe.cpp)
target_include_directories(dlssg_probe PRIVATE "${SL_SDK}/include")
# the interposer's own library stands in for d3d12.lib and dxgi.lib: device and factory creation go through it
target_link_libraries(dlssg_probe PRIVATE "${SL_SDK}/lib/x64/sl.interposer.lib" d3dcompiler)
target_compile_definitions(dlssg_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
add_custom_command(TARGET dlssg_probe POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SL_SDK}/bin/x64/sl.interposer.dll" "${SL_SDK}/bin/x64/sl.common.dll" "${SL_SDK}/bin/x64/sl.dlss_g.dll" "${SL_SDK}/bin/x64/sl.reflex.dll"
    "${SL_SDK}/bin/x64/sl.pcl.dll" "${SL_SDK}/bin/x64/nvngx_dlssg.dll" $<TARGET_FILE_DIR:dlssg_probe>)

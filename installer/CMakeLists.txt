cmake_minimum_required(VERSION 3.14)
project(LSAddonManagerSetup CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX)

# The installer's core: find Lossless Scaling, tell whose Lossless.dll is whose, install / update / repair / uninstall with backups and a rollback.
# It needs nothing but Windows.
add_library(setup_core STATIC
    src/core/fileinfo.cpp
    src/core/lsfolder.cpp
    src/core/state.cpp
    src/core/installer.cpp
    src/core/payload.cpp)
target_include_directories(setup_core PUBLIC src/core)

# Command line front end: detect, status, install, uninstall (also what the tests drive)
add_executable(setup_cli src/cli/main.cpp)
target_link_libraries(setup_cli PRIVATE setup_core)

# Stand-in DLLs for the tests: they carry a version resource and do nothing else
function(add_fake name product company version csv)
    set(FAKE_PRODUCT "${product}")
    set(FAKE_COMPANY "${company}")
    set(FAKE_VERSION "${version}")
    set(FAKE_VERSION_CSV "${csv}")
    configure_file(tests/fake.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${name}.rc @ONLY)
    add_library(${name} SHARED EXCLUDE_FROM_ALL tests/fake.cpp ${CMAKE_CURRENT_BINARY_DIR}/${name}.rc)
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")
endfunction()
add_fake(fake_original     "Lossless Scaling"   "THS"                        "3.2.2.0" "3,2,2,0")
add_fake(fake_original_new "Lossless Scaling"   "THS"                        "3.2.3.0" "3,2,3,0")
add_fake(fake_ours_old     "Echo Addon Manager" "Echo Addon Manager project" "0.1.0"   "0,1,0,0")   # the name before 0.9.1
add_fake(fake_other        "Something Else"     "Nobody"                     "1.0.0.0" "1,0,0,0")

# Offline test of the core against fake Lossless Scaling folders in %TEMP%: cmake --build . --config Release --target setup_test
add_executable(setup_test EXCLUDE_FROM_ALL tests/installer_test.cpp)
target_link_libraries(setup_test PRIVATE setup_core)
add_dependencies(setup_test fake_original fake_original_new fake_ours_old fake_other)

# Offline test of the file bundle the installer carries: cmake --build . --config Release --target setup_payload_test
add_executable(setup_payload_test EXCLUDE_FROM_ALL tests/payload_test.cpp)
target_link_libraries(setup_payload_test PRIVATE setup_core)

# ---------------------------------------------------------------------------------------------------------------------------------------
# The Setup exe: a wizard (Windows' TaskDialog) over the core, with a silent mode. The files it installs are a resource of the exe:
#   pack_payload <folder like the release zip> payload.bin, then configure with -DSETUP_PAYLOAD=<payload.bin> and build LSAddonManagerSetup.
# Without SETUP_PAYLOAD the exe carries nothing (it says so); --payload <folder> then supplies the files (tests, development).
add_executable(pack_payload tools/pack_payload.cpp)
target_link_libraries(pack_payload PRIVATE setup_core)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../manager/sdk/include/eam/version.h" _ver_line REGEX "EAM_VERSION_STRING \"")
# read again when the version changes, or a build folder keeps the old number in Setup's file version (the manager's update checks it)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../manager/sdk/include/eam/version.h")
string(REGEX REPLACE ".*\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*" "\\1" SETUP_VERSION "${_ver_line}")
string(REPLACE "." "," SETUP_VERSION_CSV "${SETUP_VERSION},0")
set(SETUP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../manager/manager-icon.ico")
set(SETUP_PAYLOAD "" CACHE FILEPATH "the file bundle to build into the Setup exe (made by pack_payload)")
if(SETUP_PAYLOAD)
    set(SETUP_HAS_PAYLOAD 1)
    set(SETUP_PAYLOAD_RC "${SETUP_PAYLOAD}")
else()
    set(SETUP_HAS_PAYLOAD 0)
    set(SETUP_PAYLOAD_RC "")
endif()
configure_file(src/gui/setup.rc.in ${CMAKE_CURRENT_BINARY_DIR}/setup.rc @ONLY)
add_executable(LSAddonManagerSetup WIN32 src/gui/main.cpp ${CMAKE_CURRENT_BINARY_DIR}/setup.rc)
target_link_libraries(LSAddonManagerSetup PRIVATE setup_core)
if(SETUP_PAYLOAD)
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/setup.rc PROPERTIES OBJECT_DEPENDS "${SETUP_PAYLOAD}")
endif()

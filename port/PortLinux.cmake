# Linux/POSIX build of the native Melee port.
#
# The recompiled guest and the simulation/HLE/network runtime are platform-independent. This file
# swaps the Windows-only pieces (D3D12, WASAPI/WinMM, WinUSB, WinHTTP, Win32 window) for SDL2,
# OpenGL, libcurl and OpenSSL. The OpenGL backend is a first cut (approximate shading; no EFB-copy
# effects or DLSS); the simulation itself is complete and runs headless.
# SPDX-License-Identifier: GPL-2.0-or-later

enable_language(C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(OPENSSL REQUIRED openssl)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
find_package(OpenGL REQUIRED)
find_package(GLEW REQUIRED)

set(PORT_GEN "${CMAKE_CURRENT_SOURCE_DIR}/generated")
if(NOT EXISTS "${PORT_GEN}/guest_sources.cmake")
  message(FATAL_ERROR "port: generated code missing; run python port/recomp/recomp.py")
endif()
include("${PORT_GEN}/guest_sources.cmake")

set(PORT_WARN_FLAGS -w)   # the generated and transcribed code is noisy; keep the build readable

# ---- guest translation units -------------------------------------------------
add_library(guest STATIC ${GUEST_SOURCES} "${PORT_GEN}/function_names.cpp" "${PORT_GEN}/gecko_data.cpp")
target_include_directories(guest PUBLIC "${PORT_GEN}" runtime/ppc)
target_include_directories(guest PRIVATE runtime/hle)
target_compile_features(guest PUBLIC cxx_std_17)
target_compile_options(guest PRIVATE ${PORT_WARN_FLAGS} -mavx2 -mfma -ffp-contract=off -O2)
target_compile_definitions(guest PRIVATE NOMINMAX)

# ---- host runtime ------------------------------------------------------------
file(GLOB RUNTIME_SOURCES runtime/ppc/*.cpp runtime/hle/*.cpp runtime/host/*.cpp runtime/gx/*.cpp)
# D3D12, DLSS and the ImGui-DX12 settings panel are Windows-only.
list(REMOVE_ITEM RUNTIME_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_d3d12.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/gx_streamline.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/pc_settings.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/threaded_backend.cpp"
  # native_animation (generated FObjHost or the same stub) provides NativeMelee::SamplePacked.
  "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gx/fobj_host_stub.cpp")

file(GLOB ENET_SOURCES third_party/enet/*.c)
list(REMOVE_ITEM ENET_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/enet/win32.c")
add_library(port_enet STATIC ${ENET_SOURCES})
target_include_directories(port_enet PUBLIC third_party/enet/include)
target_compile_options(port_enet PRIVATE -w)
target_compile_definitions(port_enet PRIVATE HAS_SOCKLEN_T=1 HAS_INET_PTON=1 HAS_INET_NTOP=1)

file(STRINGS "${PROJECT_SOURCE_DIR}/VERSION" MELEE_PORT_VERSION LIMIT_COUNT 1)

add_library(runtime STATIC ${RUNTIME_SOURCES} third_party/slippilib/SlippiGame.cpp)
target_compile_definitions(runtime PRIVATE MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")
target_include_directories(runtime PUBLIC
  runtime/ppc runtime/host runtime/hle runtime/gx "${PORT_GEN}" "${PROJECT_SOURCE_DIR}/native" third_party
  ${SDL2_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS} ${LIBUSB_INCLUDE_DIRS})
target_compile_features(runtime PUBLIC cxx_std_17)
target_compile_options(runtime PRIVATE ${PORT_WARN_FLAGS} -mavx2 -mfma -ffp-contract=off -O2)
target_compile_definitions(runtime PRIVATE NOMINMAX)
target_link_libraries(runtime PUBLIC
  native_animation port_enet
  ${SDL2_LIBRARIES} ${CURL_LIBRARIES} ${OPENSSL_LIBRARIES} ${LIBUSB_LIBRARIES}
  OpenGL::GL GLEW::GLEW pthread dl m)
target_link_directories(runtime PUBLIC ${SDL2_LIBRARY_DIRS} ${CURL_LIBRARY_DIRS} ${LIBUSB_LIBRARY_DIRS})

add_executable(melee_port app/main.cpp)
# guest and runtime reference each other (the recompiled code calls HLE entries; the runtime's
# dispatch table references guest functions), so the linker must rescan both archives.
target_link_libraries(melee_port PRIVATE "$<LINK_GROUP:RESCAN,guest,runtime>")
target_compile_definitions(melee_port PRIVATE NOMINMAX MELEE_PORT_VERSION="${MELEE_PORT_VERSION}")

# ---- unit tests that do not need an ISO -------------------------------------
add_executable(port_frame_queue_test tests/frame_queue_test.cpp)
target_include_directories(port_frame_queue_test PRIVATE runtime/gx)
target_compile_features(port_frame_queue_test PRIVATE cxx_std_17)
add_test(NAME port_frame_queue COMMAND port_frame_queue_test)

add_executable(port_memory_range_test tests/memory_range_test.cpp)
target_include_directories(port_memory_range_test PRIVATE runtime/host)
target_compile_features(port_memory_range_test PRIVATE cxx_std_17)
add_test(NAME port_memory_ranges COMMAND port_memory_range_test)

add_executable(port_texture_test tests/texture_snapshot_test.cpp runtime/gx/gx_texture.cpp)
target_include_directories(port_texture_test PRIVATE runtime/gx)
target_compile_features(port_texture_test PRIVATE cxx_std_17)
add_test(NAME port_texture_snapshots COMMAND port_texture_test)

add_executable(port_ax_ucode_test tests/ax_ucode_test.cpp runtime/hle/ax_ucode.cpp)
target_include_directories(port_ax_ucode_test PRIVATE runtime/hle)
target_compile_features(port_ax_ucode_test PRIVATE cxx_std_17)
add_test(NAME port_ax_ucode COMMAND port_ax_ucode_test)

add_executable(port_vcdiff_test tests/vcdiff_test.cpp runtime/host/vcdiff.cpp)
target_include_directories(port_vcdiff_test PRIVATE runtime/host)
target_compile_features(port_vcdiff_test PRIVATE cxx_std_17)
add_test(NAME port_vcdiff COMMAND port_vcdiff_test)

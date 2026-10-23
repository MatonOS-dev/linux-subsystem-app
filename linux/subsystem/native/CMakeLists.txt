cmake_minimum_required(VERSION 3.22.1)
project(maton_compositor LANGUAGES C CXX)

if(NOT MATON_COMPOSITOR_ROOT)
  message(FATAL_ERROR "MATON_COMPOSITOR_ROOT must point to linux/subsystem")
endif()
get_filename_component(MATON_COMPOSITOR_ROOT "${MATON_COMPOSITOR_ROOT}" ABSOLUTE)
if(NOT MATON_AOSP_ROOT)
  get_filename_component(MATON_AOSP_ROOT "${MATON_COMPOSITOR_ROOT}/../../../../../" ABSOLUTE)
endif()
if(NOT MATON_COMPOSITOR_BUILD)
  set(MATON_COMPOSITOR_BUILD "${MATON_AOSP_ROOT}/out/matonos/compositor")
endif()
set(MATON_STAGE "${MATON_COMPOSITOR_BUILD}/stage/x86_64")
set(MATON_XSTAGE "${MATON_COMPOSITOR_BUILD}/xstage/x86_64")
set(MATON_WAYLAND_SCANNER "${MATON_COMPOSITOR_BUILD}/host/bin/wayland-scanner")
set(MATON_XDG_XML "${MATON_COMPOSITOR_BUILD}/host/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml")
set(MATON_AHB_XML "${MATON_COMPOSITOR_ROOT}/native/protocol/maton-ahb-buffer-v1.xml")
set(MATON_WLROOTS_INCLUDE "${MATON_COMPOSITOR_BUILD}/sources/src/wlroots-0.20.2/include")

foreach(required "${MATON_WAYLAND_SCANNER}" "${MATON_XDG_XML}" "${MATON_AHB_XML}")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "Missing ${required}; build the pinned compositor dependencies first")
  endif()
endforeach()

set(GENERATED "${MATON_COMPOSITOR_BUILD}/generated")
file(MAKE_DIRECTORY "${GENERATED}")
add_custom_command(OUTPUT "${GENERATED}/xdg-shell-client-protocol.h"
  COMMAND "${MATON_WAYLAND_SCANNER}" client-header "${MATON_XDG_XML}" "${GENERATED}/xdg-shell-client-protocol.h"
  DEPENDS "${MATON_XDG_XML}" VERBATIM)
add_custom_command(OUTPUT "${GENERATED}/xdg-shell-protocol.c"
  COMMAND "${MATON_WAYLAND_SCANNER}" private-code "${MATON_XDG_XML}" "${GENERATED}/xdg-shell-protocol.c"
  DEPENDS "${MATON_XDG_XML}" VERBATIM)
add_custom_command(OUTPUT "${GENERATED}/wayland-protocols/xdg-shell-enum.h"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GENERATED}/wayland-protocols"
  COMMAND "${MATON_WAYLAND_SCANNER}" enum-header "${MATON_XDG_XML}" "${GENERATED}/wayland-protocols/xdg-shell-enum.h"
  DEPENDS "${MATON_XDG_XML}" VERBATIM)
add_custom_command(OUTPUT "${GENERATED}/maton-ahb-buffer-v1-server-protocol.h"
  COMMAND "${MATON_WAYLAND_SCANNER}" server-header "${MATON_AHB_XML}" "${GENERATED}/maton-ahb-buffer-v1-server-protocol.h"
  DEPENDS "${MATON_AHB_XML}" VERBATIM)
add_custom_command(OUTPUT "${GENERATED}/maton-ahb-buffer-v1-protocol.c"
  COMMAND "${MATON_WAYLAND_SCANNER}" private-code "${MATON_AHB_XML}" "${GENERATED}/maton-ahb-buffer-v1-protocol.c"
  DEPENDS "${MATON_AHB_XML}" VERBATIM)

foreach(lib wlroots wayland-server wayland-client pixman-1 xkbcommon drm)
  if(lib STREQUAL "wlroots")
    find_library(MATON_${lib}_LIB NAMES wlroots-0.20 wlroots PATHS "${MATON_STAGE}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  else()
    find_library(MATON_${lib}_LIB NAMES ${lib} PATHS "${MATON_STAGE}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
  endif()
  add_library(maton_${lib} SHARED IMPORTED)
  set_target_properties(maton_${lib} PROPERTIES IMPORTED_LOCATION "${MATON_${lib}_LIB}")
endforeach()

add_library(maton_compositor SHARED
  compositor_core.c android_keymap.c jni_shim.cpp android_buffer.c ahb_buffer.c android_output.c dmabuf_import.c dmabuf_metadata.cpp maton_renderer.c presenter.c test_client.c
  "${GENERATED}/xdg-shell-protocol.c" "${GENERATED}/xdg-shell-client-protocol.h"
  "${GENERATED}/wayland-protocols/xdg-shell-enum.h"
  "${GENERATED}/maton-ahb-buffer-v1-protocol.c" "${GENERATED}/maton-ahb-buffer-v1-server-protocol.h")
target_compile_features(maton_compositor PRIVATE c_std_11 cxx_std_17)
target_compile_options(maton_compositor PRIVATE -march=x86-64-v2)
target_compile_definitions(maton_compositor PRIVATE WLR_USE_UNSTABLE)
target_include_directories(maton_compositor PRIVATE
  "${GENERATED}"
  "${MATON_COMPOSITOR_ROOT}/native"
  "${MATON_WLROOTS_INCLUDE}"
  "${MATON_STAGE}/include"
  "${MATON_STAGE}/include/pixman-1"
  "${MATON_XSTAGE}/include"
  "${MATON_COMPOSITOR_ROOT}/../../third_party/platform-headers/include")
find_library(MATON_XCB_LIB NAMES xcb PATHS "${MATON_XSTAGE}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH REQUIRED)
target_link_libraries(maton_compositor PRIVATE
  "${MATON_XCB_LIB}"
  maton_wlroots maton_wayland-server maton_wayland-client maton_pixman-1 maton_xkbcommon maton_drm
  android log EGL GLESv2 dl)

if(NOT MATON_PIPEWIRE_BUILD)
  set(MATON_PIPEWIRE_BUILD "${MATON_AOSP_ROOT}/out/matonos/pipewire")
endif()
add_library(maton_pipewire SHARED pipewire_android.c)
target_compile_features(maton_pipewire PRIVATE c_std_11)
target_compile_options(maton_pipewire PRIVATE -march=x86-64-v2)
target_compile_definitions(maton_pipewire PRIVATE _GNU_SOURCE)
target_include_directories(maton_pipewire PRIVATE
  "${MATON_PIPEWIRE_BUILD}/install/pipewire/include/pipewire-0.3"
  "${MATON_PIPEWIRE_BUILD}/install/pipewire/include/spa-0.2")
target_link_libraries(maton_pipewire PRIVATE
  "${MATON_PIPEWIRE_BUILD}/jniLibs/x86_64/libpipewire-0.3.so" aaudio log dl)

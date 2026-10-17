# libbloodframe.so (Bloodframe, based on lsfg-vk 1.0 by PancakeTAS, MIT, via GameNative's Android fork):
# the Lossless Scaling frame generation of the fork (framegen backend, Lossless.dll shader extraction,
# DXBC -> SPIR-V) behind the small C API of bb_lsfg.h, for the Bionic game process. Not the Vulkan layer: libbbgpu calls it directly (build_bloodframe.sh).
# -DLSFG_SRC=<the fork's source tree, with our patches applied>
cmake_minimum_required(VERSION 3.18)
project(bloodframe LANGUAGES C CXX)
if(NOT LSFG_SRC OR NOT EXISTS "${LSFG_SRC}/framegen/CMakeLists.txt")
  message(FATAL_ERROR "LSFG_SRC is not the lsfg-vk-android source tree")
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(BUILD_SHARED_LIBS OFF)
# The same flags the fork's top level gives its subprojects.
add_compile_options(-Wno-deprecated-declarations -Wno-unused-template)
# VK_ANDROID_external_memory_android_hardware_buffer types (the fork's layer gets them from its
# volk/Vulkan setup; framegen includes the plain headers).
add_compile_definitions(VK_USE_PLATFORM_ANDROID_KHR)

add_subdirectory(${LSFG_SRC}/thirdparty/dxbc dxbc EXCLUDE_FROM_ALL)
add_subdirectory(${LSFG_SRC}/thirdparty/pe-parse/pe-parser-library pe-parse EXCLUDE_FROM_ALL)
add_subdirectory(${LSFG_SRC}/thirdparty/volk volk EXCLUDE_FROM_ALL)
add_subdirectory(${LSFG_SRC}/framegen framegen EXCLUDE_FROM_ALL)

# framegen/v3.1*_src/context.cpp call close() before their <unistd.h>; libc++ of the NDK does not
# bring it in.
target_compile_options(lsfg-vk-framegen PRIVATE -include unistd.h)

add_library(bloodframe SHARED
  bb_lsfg.cpp
  ${LSFG_SRC}/src/extract/extract.cpp
  ${LSFG_SRC}/src/extract/trans.cpp)
set_target_properties(bloodframe PROPERTIES
  OUTPUT_NAME bloodframe
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)
target_include_directories(bloodframe PRIVATE ${LSFG_SRC}/include)
# nativewindow: AHardwareBuffer (not libandroid.so, which pulls in the framework); log: logcat.
target_link_libraries(bloodframe PRIVATE lsfg-vk-framegen pe-parse dxbc volk nativewindow log)
# Only the C API leaves the library; the static archives' symbols stay inside (volk's global
# vkCreateInstance etc. must not interpose on anything).
target_link_options(bloodframe PRIVATE
  -Wl,--exclude-libs,ALL
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/bb_lsfg.map)

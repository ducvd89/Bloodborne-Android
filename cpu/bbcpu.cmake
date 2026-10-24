# libbbcpu.so: guest_cpu.h on FEXCore, linked statically against FEX's libraries. guest_cpu_aot.cpp exports guest_cpu.h and
# runs ahead-of-time units (BB_AOT=<bb-recomp .so>); guest_cpu_fex.cpp, compiled as the fallback (BB_AOT_FALLBACK renames its
# guest_cpu_* to bb_fb_*), runs everything the units do not cover on the same FEX thread state. Without BB_AOT it is FEX alone.
set(BBCPU_DIR "${CMAKE_CURRENT_LIST_DIR}")
add_library(bbcpu SHARED "${BBCPU_DIR}/guest_cpu_aot.cpp" "${BBCPU_DIR}/guest_cpu_fex.cpp" "${BBCPU_DIR}/host_call_arm64.S")
target_compile_definitions(bbcpu PRIVATE BB_AOT_FALLBACK=1 BB_AOT_FEX_STATE=1)
target_include_directories(bbcpu PRIVATE "${BBCPU_DIR}" "${BBCPU_DIR}/../src" "${CMAKE_SOURCE_DIR}/Source" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(bbcpu PRIVATE FEXCore Common JemallocLibs fmt::fmt)
target_compile_options(bbcpu PRIVATE ${FEX_TUNE_COMPILE_FLAGS})
set_target_properties(bbcpu PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN TRUE)

if(ANDROID)
  # libc++ (Bionic's only C++ library) does not satisfy fextl::move_only_function's noexcept
  # std::function assignment, which FEXServerClient.cpp needs. This library runs no FEXServer, so
  # that file stays out of FEX's Common; the link tells if anything still needs it.
  get_target_property(bb_common_sources Common SOURCES)
  list(FILTER bb_common_sources EXCLUDE REGEX "FEXServerClient\\.cpp$")
  set_target_properties(Common PROPERTIES SOURCES "${bb_common_sources}")
endif()

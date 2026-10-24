# bb-recomp target, inside FEX's build. FEXCore's per-target settings (the DEFINES list in
# FEXCore/Source/CMakeLists.txt) change header layouts, so they are repeated here.
set(BBR "${BB_RECOMP_DIR}/src")
# FEX's allocator hooks: JemallocDummy (the glibc allocator) on Linux, JemallocLibs (FEX's rpmalloc allocator) on MinGW
set(BBR_JEMALLOC_DUMMY "")
if(TARGET JemallocDummy)
  set(BBR_JEMALLOC_DUMMY JemallocDummy)
elseif(TARGET JemallocLibs)
  set(BBR_JEMALLOC_DUMMY JemallocLibs)
endif()
set(BBR_COMMON "${BBR}/elf_image.cpp" "${BBR}/fde_scan.cpp" "${BBR}/lifter.cpp" "${BBR}/jumptable.cpp" "${BBR}/eh_frame.cpp" "${BBR}/discover.cpp")
set(BBR_JIT "${BBR}/jit_host.cpp")
set(BBR_HELPERS "${BB_RECOMP_DIR}/helpers")
# The x87 helpers (plain C, one translation unit) are linked into every tool: the in-process JIT of the test tools calls them
# directly. The same file is compiled again for the target and linked into the generated .so (compile.cpp).
set_source_files_properties("${BBR_HELPERS}/bbr_x87_all.c" PROPERTIES LANGUAGE C COMPILE_OPTIONS "-O2;-fvisibility=hidden;-fno-stack-protector;-ffp-contract=off")
set(BBR_LLVM_SRCS "${BBR}/lower_llvm.cpp" "${BBR}/lower_ops.cpp" "${BBR}/lower_vector.cpp" "${BBR}/codegen.cpp" "${BBR}/compile.cpp" "${BBR}/bundle.cpp")
add_executable(bb-recomp "${BBR}/main.cpp" ${BBR_COMMON} "${BBR}/census.cpp")
target_include_directories(bb-recomp PRIVATE
  "${CMAKE_SOURCE_DIR}/FEXCore/Source" "${CMAKE_SOURCE_DIR}/FEXCore/include" "${CMAKE_BINARY_DIR}/include" "${BBR}" "${BBR_HELPERS}")
target_compile_definitions(bb-recomp PRIVATE
  JIT_ARM64 ARCHITECTURE_x86_64=1 FEXCORE_PRESERVE_ALL_ATTR= FEXCORE_HAS_PRESERVE_ALL_ATTR=0)
target_compile_options(bb-recomp PRIVATE -Wall -Wno-trigraphs -fwrapv -march=${BB_RECOMP_MARCH})
target_link_libraries(bb-recomp PRIVATE FEXCore FEXCore_Base CodeEmitter FEXHeaderUtils ${BBR_JEMALLOC_DUMMY} fmt::fmt pthread)
add_dependencies(bb-recomp IR_INC CONFIG_INC)
set_target_properties(bb-recomp PROPERTIES CXX_STANDARD 20 CXX_VISIBILITY_PRESET hidden)

if(BB_RECOMP_LLVM)
  # Step 2: the system LLVM (AArch64 and X86 targets) as a shared library.
  if(BB_RECOMP_LLVM_DIR)
    set(LLVM_DIR "${BB_RECOMP_LLVM_DIR}")
  endif()
  if(BB_RECOMP_DEPS_PREFIX)
    # LLVM's CMake files find zlib and zstd again (static libraries from the prefix)
    list(APPEND CMAKE_PREFIX_PATH "${BB_RECOMP_DEPS_PREFIX}")
    set(ZLIB_USE_STATIC_LIBS ON)
  endif()
  find_package(LLVM REQUIRED CONFIG)
  message(STATUS "bb-recomp: LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
  set(BBR_TOOLS bb-recomp bb-recomp-asmtest bb-recomp-difftest)
  if(BB_RECOMP_BUNDLED)
    set(BBR_TOOLS bb-recomp)
  endif()
  foreach(t ${BBR_TOOLS})
    if(NOT t STREQUAL "bb-recomp")
      # the host-side test tools: the lifter, the lowering and the in-process JIT, no census
      if(t STREQUAL "bb-recomp-asmtest")
        add_executable(${t} "${BBR}/asmtest.cpp" ${BBR_COMMON} ${BBR_JIT})
      else()
        add_executable(${t} "${BBR}/difftest.cpp" ${BBR_COMMON} ${BBR_JIT})
      endif()
      target_include_directories(${t} PRIVATE
        "${CMAKE_SOURCE_DIR}/FEXCore/Source" "${CMAKE_SOURCE_DIR}/FEXCore/include" "${CMAKE_BINARY_DIR}/include" "${BBR}" "${BBR_HELPERS}")
      target_compile_definitions(${t} PRIVATE
        JIT_ARM64 ARCHITECTURE_x86_64=1 FEXCORE_PRESERVE_ALL_ATTR= FEXCORE_HAS_PRESERVE_ALL_ATTR=0)
      target_compile_options(${t} PRIVATE -Wall -Wno-trigraphs -fwrapv -march=${BB_RECOMP_MARCH})
      target_link_libraries(${t} PRIVATE FEXCore FEXCore_Base CodeEmitter FEXHeaderUtils ${BBR_JEMALLOC_DUMMY} fmt::fmt pthread)
      add_dependencies(${t} IR_INC CONFIG_INC)
      set_target_properties(${t} PROPERTIES CXX_STANDARD 20 CXX_VISIBILITY_PRESET hidden)
    endif()
    target_sources(${t} PRIVATE ${BBR_LLVM_SRCS} "${BBR_HELPERS}/bbr_x87_all.c")
    target_compile_definitions(${t} PRIVATE BBR_HELPERS_DIR="${BBR_HELPERS}")
    target_include_directories(${t} PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(${t} PRIVATE BB_RECOMP_LLVM=1 ${LLVM_DEFINITIONS})
    if(TARGET LLVM)
      target_link_libraries(${t} PRIVATE LLVM)
    else()
      llvm_map_components_to_libnames(BB_LLVM_LIBS core support irreader passes object orcjit native aarch64codegen aarch64asmparser x86codegen)
      target_link_libraries(${t} PRIVATE ${BB_LLVM_LIBS})
    endif()
  endforeach()
  if(NOT BB_RECOMP_BUNDLED)
    set(BB_RECOMP_TEST_NASM "${BB_RECOMP_NASM}" CACHE STRING "")
    target_compile_definitions(bb-recomp-asmtest PRIVATE BBR_NASM="${BB_RECOMP_NASM}" BBR_FEX_SRC="${CMAKE_SOURCE_DIR}")
  endif()
  if(BB_RECOMP_BUNDLED)
    # ld.lld in the process (bundle.cpp): LLD's ELF driver as a static library, and a fully static executable (no glibc or
    # libstdc++ version needed on the player's machine). The libraries are linked by path: LLD's CMake target would pull in every
    # LLVM target, which the ELF driver does not need without LTO.
    llvm_map_components_to_libnames(BB_LLD_DEPS option targetparser binaryformat demangle lto dtlto transformutils profiledata bitwriter bitreader mc debuginfodwarf aarch64asmparser x86asmparser aarch64codegen x86codegen passes object)
    target_compile_definitions(bb-recomp PRIVATE BBR_BUNDLED=1)
    if(BB_RECOMP_LLD_INCLUDE)
      target_include_directories(bb-recomp PRIVATE "${BB_RECOMP_LLD_INCLUDE}")
    endif()
    target_link_libraries(bb-recomp PRIVATE -Wl,--start-group "${LLVM_LIBRARY_DIR}/liblldELF.a" "${LLVM_LIBRARY_DIR}/liblldCommon.a" ${BB_LLD_DEPS} -Wl,--end-group)
    # Fully static (no glibc needed) for the command line only build; with the window on Linux SDL loads X11, Wayland and GL with dlopen,
    # which a static glibc cannot do: glibc stays dynamic (the toolchain file builds against glibc 2.35), libstdc++ and libgcc static.
    if(WIN32 OR NOT BB_RECOMP_SDL_PREFIX)
      target_link_options(bb-recomp PRIVATE -static)
    endif()
  endif()
endif()

# Player mode: bb-prepare's own sources (tools/android/prepare) are compiled into bb-recomp, with its main() renamed, so the game image
# the PC tool builds is what the phone builds. player.cpp drives it (and recompMain) for `bb-recomp --game <dump folder>`.
if(BB_RECOMP_LLVM AND BB_RECOMP_PLAYER)
  get_filename_component(BBR_PREPARE "${BB_RECOMP_DIR}/../android/prepare" ABSOLUTE)
  file(GLOB BBR_PREPARE_SRCS "${BBR_PREPARE}/*.cpp")
  if(WIN32)
    # io.cpp (mmap, open, dirent) is replaced by the Win32 version of the same interface
    list(REMOVE_ITEM BBR_PREPARE_SRCS "${BBR_PREPARE}/io.cpp")
    list(APPEND BBR_PREPARE_SRCS "${BB_RECOMP_DIR}/windows/io_win.cpp")
  endif()
  target_sources(bb-recomp PRIVATE ${BBR_PREPARE_SRCS} "${BBR}/player.cpp" "${BBR}/pkgs.cpp" "${BBR}/proc.cpp")
  set_source_files_properties("${BBR_PREPARE}/bb_prepare.cpp" PROPERTIES COMPILE_DEFINITIONS "main=bb_prepare_main")
  # Their headers (hash.h, io.h ...) have generic names: they are found by quoted include only (-iquote), never as <io.h> (Windows has one).
  set_source_files_properties("${BBR}/player.cpp" "${BBR}/pkgs.cpp" PROPERTIES COMPILE_OPTIONS "-iquote;${BBR_PREPARE}")
  if(WIN32)
    set_source_files_properties("${BB_RECOMP_DIR}/windows/io_win.cpp" PROPERTIES COMPILE_OPTIONS "-iquote;${BBR_PREPARE}")
  endif()
  target_compile_definitions(bb-recomp PRIVATE BB_RECOMP_PLAYER=1 BBR_TOOL_VERSION="${BB_RECOMP_VERSION}")
  find_package(ZLIB REQUIRED)
  target_link_libraries(bb-recomp PRIVATE ZLIB::ZLIB)
endif()

# The window: Dear ImGui (SDL3 + SDL_Renderer backends) on a static SDL3. The notices shown in About are NOTICES, compiled in.
if(BB_RECOMP_LLVM AND BB_RECOMP_PLAYER AND BB_RECOMP_SDL_PREFIX AND BB_RECOMP_IMGUI_DIR)
  list(APPEND CMAKE_PREFIX_PATH "${BB_RECOMP_SDL_PREFIX}")
  if(WIN32 OR NOT EXISTS "${BB_RECOMP_SDL_PREFIX}/lib/libSDL3.a")
    find_package(SDL3 REQUIRED CONFIG)
  endif()
  file(READ "${BB_RECOMP_DIR}/NOTICES" BBR_NOTICES)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BB_RECOMP_DIR}/NOTICES")
  file(WRITE "${CMAKE_BINARY_DIR}/notices_data.cpp"
    "// generated from tools/recomp/NOTICES\nextern const char* const kNoticesText;\nconst char* const kNoticesText = R\"BBN(${BBR_NOTICES})BBN\";\n")
  set(BBR_IMGUI "${BB_RECOMP_IMGUI_DIR}")
  target_sources(bb-recomp PRIVATE "${BBR}/gui.cpp" "${CMAKE_BINARY_DIR}/notices_data.cpp"
    "${BBR_IMGUI}/imgui.cpp" "${BBR_IMGUI}/imgui_draw.cpp" "${BBR_IMGUI}/imgui_tables.cpp" "${BBR_IMGUI}/imgui_widgets.cpp"
    "${BBR_IMGUI}/backends/imgui_impl_sdl3.cpp" "${BBR_IMGUI}/backends/imgui_impl_sdlrenderer3.cpp")
  set_source_files_properties("${BBR}/gui.cpp" PROPERTIES COMPILE_OPTIONS "-iquote;${BBR_IMGUI}")
  target_include_directories(bb-recomp PRIVATE "${BBR_IMGUI}")
  target_compile_definitions(bb-recomp PRIVATE BB_RECOMP_GUI=1)
  if(NOT WIN32 AND EXISTS "${BB_RECOMP_SDL_PREFIX}/lib/libSDL3.a")
    # Linux: the static SDL3 as a plain library. Its CMake package would also ask for libdecor and fribidi, which SDL loads with dlopen at
    # run time (SDL_DEPS_SHARED) and the build machine does not need.
    target_include_directories(bb-recomp PRIVATE "${BB_RECOMP_SDL_PREFIX}/include")
    target_link_libraries(bb-recomp PRIVATE "${BB_RECOMP_SDL_PREFIX}/lib/libSDL3.a" m dl pthread rt)
  elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(bb-recomp PRIVATE SDL3::SDL3-static)
  else()
    target_link_libraries(bb-recomp PRIVATE SDL3::SDL3)
  endif()
endif()

if(BB_RECOMP_MIMALLOC_DIR)
  target_sources(bb-recomp PRIVATE "${BB_RECOMP_MIMALLOC_DIR}/src/static.c" "${BBR}/alloc_override.cpp")
  target_include_directories(bb-recomp PRIVATE "${BB_RECOMP_MIMALLOC_DIR}/include")
  set_source_files_properties("${BB_RECOMP_MIMALLOC_DIR}/src/static.c" PROPERTIES COMPILE_OPTIONS "-O2;-fno-lto;-DMI_MALLOC_OVERRIDE=0;-DMI_SECURE=0;-DNDEBUG")
endif()

# Windows (llvm-mingw): the POSIX names the shared sources use (windows/posix_shim.h), a static executable.
if(WIN32)
  foreach(t bb-recomp bb-recomp-asmtest bb-recomp-difftest)
    if(TARGET ${t})
      target_compile_options(${t} PRIVATE -include "${BB_RECOMP_DIR}/windows/posix_shim.h")
    endif()
  endforeach()
  target_link_options(bb-recomp PRIVATE -static)
  # UTF-8 as the code page (windows/utf8.manifest): the game and output folders may have accents in their names
  execute_process(COMMAND "${CMAKE_RC_COMPILER}" utf8.rc -O coff -o "${CMAKE_BINARY_DIR}/utf8.o" WORKING_DIRECTORY "${BB_RECOMP_DIR}/windows")
  target_link_libraries(bb-recomp PRIVATE "${CMAKE_BINARY_DIR}/utf8.o")
endif()

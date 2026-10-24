# bb-recomp target, inside FEX's build. FEXCore's per-target settings (the DEFINES list in
# FEXCore/Source/CMakeLists.txt) change header layouts, so they are repeated here.
set(BBR "${BB_RECOMP_DIR}/src")
set(BBR_COMMON "${BBR}/elf_image.cpp" "${BBR}/fde_scan.cpp" "${BBR}/lifter.cpp" "${BBR}/jumptable.cpp" "${BBR}/eh_frame.cpp" "${BBR}/discover.cpp" "${BBR}/variants.cpp")
set(BBR_JIT "${BBR}/jit_host.cpp")
set(BBR_HELPERS "${BB_RECOMP_DIR}/helpers")
# The x87 helpers (plain C, one translation unit) are linked into every tool: the in-process JIT of the test tools calls them
# directly. The same file is compiled again for the target and linked into the generated .so (compile.cpp).
set_source_files_properties("${BBR_HELPERS}/bbr_x87_all.c" PROPERTIES LANGUAGE C COMPILE_OPTIONS "-O2;-fvisibility=hidden;-fno-stack-protector;-ffp-contract=off")
set(BBR_LLVM_SRCS "${BBR}/lower_llvm.cpp" "${BBR}/lower_ops.cpp" "${BBR}/lower_vector.cpp" "${BBR}/codegen.cpp" "${BBR}/compile.cpp" "${BBR}/variant_compile.cpp")
add_executable(bb-recomp "${BBR}/main.cpp" ${BBR_COMMON} "${BBR}/census.cpp")
target_include_directories(bb-recomp PRIVATE
  "${CMAKE_SOURCE_DIR}/FEXCore/Source" "${CMAKE_SOURCE_DIR}/FEXCore/include" "${CMAKE_BINARY_DIR}/include" "${BBR}" "${BBR_HELPERS}")
target_compile_definitions(bb-recomp PRIVATE
  JIT_ARM64 ARCHITECTURE_x86_64=1 FEXCORE_PRESERVE_ALL_ATTR= FEXCORE_HAS_PRESERVE_ALL_ATTR=0)
target_compile_options(bb-recomp PRIVATE -Wall -Wno-trigraphs -fwrapv -march=${BB_RECOMP_MARCH})
target_link_libraries(bb-recomp PRIVATE FEXCore FEXCore_Base CodeEmitter FEXHeaderUtils JemallocDummy fmt::fmt pthread)
add_dependencies(bb-recomp IR_INC CONFIG_INC)
set_target_properties(bb-recomp PROPERTIES CXX_STANDARD 20 CXX_VISIBILITY_PRESET hidden)

if(BB_RECOMP_LLVM)
  # Step 2: the system LLVM (AArch64 and X86 targets) as a shared library.
  find_package(LLVM REQUIRED CONFIG)
  message(STATUS "bb-recomp: LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
  foreach(t bb-recomp bb-recomp-asmtest bb-recomp-difftest)
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
      target_link_libraries(${t} PRIVATE FEXCore FEXCore_Base CodeEmitter FEXHeaderUtils JemallocDummy fmt::fmt pthread)
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
  set(BB_RECOMP_TEST_NASM "${BB_RECOMP_NASM}" CACHE STRING "")
  target_compile_definitions(bb-recomp-asmtest PRIVATE BBR_NASM="${BB_RECOMP_NASM}" BBR_FEX_SRC="${CMAKE_SOURCE_DIR}")
endif()

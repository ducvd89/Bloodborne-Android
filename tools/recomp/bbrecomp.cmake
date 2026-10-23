# bb-recomp target, inside FEX's build. FEXCore's per-target settings (the DEFINES list in
# FEXCore/Source/CMakeLists.txt) change header layouts, so they are repeated here.
set(BBR "${BB_RECOMP_DIR}/src")
add_executable(bb-recomp
  "${BBR}/main.cpp" "${BBR}/elf_image.cpp" "${BBR}/fde_scan.cpp" "${BBR}/lifter.cpp" "${BBR}/census.cpp")
target_include_directories(bb-recomp PRIVATE
  "${CMAKE_SOURCE_DIR}/FEXCore/Source" "${CMAKE_SOURCE_DIR}/FEXCore/include" "${CMAKE_BINARY_DIR}/include" "${BBR}")
target_compile_definitions(bb-recomp PRIVATE
  JIT_ARM64 ARCHITECTURE_x86_64=1 FEXCORE_PRESERVE_ALL_ATTR= FEXCORE_HAS_PRESERVE_ALL_ATTR=0)
target_compile_options(bb-recomp PRIVATE -Wall -Wno-trigraphs -fwrapv -march=${BB_RECOMP_MARCH})
target_link_libraries(bb-recomp PRIVATE FEXCore FEXCore_Base CodeEmitter FEXHeaderUtils JemallocDummy fmt::fmt pthread)
add_dependencies(bb-recomp IR_INC CONFIG_INC)
set_target_properties(bb-recomp PROPERTIES CXX_STANDARD 20 CXX_VISIBILITY_PRESET hidden)

if(BB_RECOMP_LLVM)
  # Step 2: system LLVM (AArch64 target), static libraries.
  find_package(LLVM REQUIRED CONFIG)
  llvm_map_components_to_libnames(BB_LLVM_LIBS core support irreader passes aarch64codegen aarch64asminfo aarch64desc aarch64info)
  target_include_directories(bb-recomp PRIVATE ${LLVM_INCLUDE_DIRS})
  target_compile_definitions(bb-recomp PRIVATE BB_RECOMP_LLVM=1)
  target_link_libraries(bb-recomp PRIVATE ${BB_LLVM_LIBS})
endif()

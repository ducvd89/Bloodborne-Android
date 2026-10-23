# Included by FEX's project() (CMAKE_PROJECT_FEX_INCLUDE, see ../CMakeLists.txt): bb-recomp is added to
# FEX's build once all of FEX's targets exist.
cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY \"${CMAKE_SOURCE_DIR}\" CALL include \"${CMAKE_CURRENT_LIST_DIR}/bbrecomp.cmake\")")

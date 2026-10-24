# bbport online module, second generation: libbbnet20.so. shadPS4 v0.20.0's network, NP and
# shadNet sources (vendor/shadps4, 529ceb3, with vendor-changes.diff) and The Hunter's Dream
# backend (thd/, from droogie/bbhost), as Supermedo/Awaken put them together
# (SHADNET-PORT.md). Loaded instead of libbbnet.so with BB_NET_MODULE (the launcher's pools
# "shadnet" and "huntersdream"); the shadp2p module (../bbnet, BBSC) is untouched. Hidden
# symbols and -Bsymbolic as there: the two modules never meet each other or libbbgpu.

set(BBPORT_VERSION "0.6-pre1")
set(BBNET_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/shadps4")
set(SHADNET_PROTO_OUT20 "${CMAKE_CURRENT_BINARY_DIR}/shadnet_proto_gen")
file(MAKE_DIRECTORY "${SHADNET_PROTO_OUT20}")
add_custom_command(
    OUTPUT "${SHADNET_PROTO_OUT20}/shadnet.pb.cc" "${SHADNET_PROTO_OUT20}/shadnet.pb.h"
    COMMAND $<TARGET_FILE:protobuf::protoc> "--proto_path=${BBNET_VENDOR}/shadnet"
            "--cpp_out=${SHADNET_PROTO_OUT20}" "${BBNET_VENDOR}/shadnet/shadnet.proto"
    DEPENDS "${BBNET_VENDOR}/shadnet/shadnet.proto")
file(GLOB_RECURSE BBNET20_SOURCES CONFIGURE_DEPENDS
    vendor/shadps4/core/libraries/net/*.cpp vendor/shadps4/core/libraries/network/*.cpp
    vendor/shadps4/core/libraries/np/*.cpp vendor/shadps4/core/net/*.cpp
    vendor/shadps4/shadnet/*.cpp)
# Trophies, commerce and the profile and Facebook dialogs stay in the C runtime.
list(FILTER BBNET20_SOURCES EXCLUDE REGEX
    "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")
# BBSC, Bloodborne seamless co-op (GPL-2.0+): its game code and the port's stand-ins for
# shadPS4's UI (bbsc/bbsc_glue.cpp). Installed only for Bloodborne with BB_BBSC=1 (bbnet_image_ready: with it
# unset nothing of BBSC hooks or writes the game's code). Its hooks and byte patches go through the port's
# code-hook layer in libbbgpu (shim/bbport_guest_code_hooks.h: trampolines that reach the host through the
# guest CPU, code invalidation after every write); BB_NET_BBSC=OFF builds the module without BBSC
# (bbsc_off.cpp: no-op stand-ins for what the network code reports to it).
option(BB_NET_BBSC "Build BBSC, Bloodborne seamless co-op, into the online module" ON)
if (BB_NET_BBSC)
    file(GLOB BBNET20_BBSC_SOURCES CONFIGURE_DEPENDS bbsc/core/*.cpp bbsc/bbsc_glue.cpp)
else()
    set(BBNET20_BBSC_SOURCES bbsc_off.cpp)
endif()
file(GLOB BBNET20_THD_SOURCES CONFIGURE_DEPENDS thd/*.cpp compat/common/*.cpp)

add_library(bbnet20 SHARED ${BBNET20_SOURCES} ${BBNET20_THD_SOURCES} ${BBNET20_BBSC_SOURCES}
    "${SHADNET_PROTO_OUT20}/shadnet.pb.cc" bbnet_glue.cpp bbnet_status.cpp)
target_include_directories(bbnet20 PRIVATE include bbsc compat . .. ../shim ../shadps4 "${BBNET_VENDOR}"
    "${BBNET_VENDOR}/shadnet" "${SHADNET_PROTO_OUT20}" ../third_party/imgui ../third_party/httplib)
# The version the shadNet client reports to the server (BB_CLIENT_VERSION overrides it).
target_compile_definitions(bbnet20 PRIVATE BOOST_ASIO_STANDALONE NDEBUG
    CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT BBNET_PORT_VERSION="${BBPORT_VERSION}")
target_compile_options(bbnet20 PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden
    -Wno-deprecated-declarations -Wno-missing-field-initializers)
if (BB_NET_BBSC)
    target_compile_definitions(bbnet20 PRIVATE BBNET_BBSC=1)
endif()
target_link_options(bbnet20 PRIVATE -Wl,-Bsymbolic -Wl,--exclude-libs,ALL)
if (ANDROID)
    # Only bbnet.h's C interface leaves the library (check_bionic_symbols.sh verifies it).
    target_link_options(bbnet20 PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/bbnet.map")
    set_target_properties(bbnet20 PROPERTIES LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bbnet.map")
endif()
if (ANDROID)
    # Its guest_cpu_* calls come from libbbcpu.so and its runtime_* calls from bb-probe, both in the
    # process before it is loaded (the NDK's --no-undefined would refuse them: check_bionic_symbols.sh
    # verifies every one is provided). libbbgpu.so is a real dependency (logging).
    target_link_options(bbnet20 PRIVATE -Wl,-z,undefs -Wl,--allow-shlib-undefined)
    # Android's linker resolves a dlopen'ed library's symbols from the global group (the executable, LD_PRELOADs) and
    # the library's own DT_NEEDED closure only: bb-probe's NEEDED libbbcpu.so is not visible to it, so guest_cpu_call
    # would fail the dlopen ("cannot locate symbol"). libbbnet20.so therefore has NEEDED libbbcpu.so itself
    # (BB_CPU_LIB: the libbbcpu.so build_bionic.sh built; check_bionic_symbols.sh verifies every symbol resolves).
    set(BB_CPU_LIB "" CACHE FILEPATH "libbbcpu.so the online module is linked against on Android")
    if (NOT BB_CPU_LIB OR NOT EXISTS "${BB_CPU_LIB}")
        message(FATAL_ERROR "bbnet20: BB_CPU_LIB (libbbcpu.so) is required on Android (tools/android/build_bionic.sh passes it)")
    endif()
    add_library(bbcpu_runtime SHARED IMPORTED)
    set_target_properties(bbcpu_runtime PROPERTIES IMPORTED_LOCATION "${BB_CPU_LIB}" IMPORTED_SONAME "libbbcpu.so")
    target_link_libraries(bbnet20 PRIVATE bbcpu_runtime)
endif()
set_target_properties(bbnet20 PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_libraries(bbnet20 PRIVATE bbgpu nlohmann_json::nlohmann_json protobuf::libprotobuf
    OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB "${MINIUPNPC_LIBRARY}" Vulkan::Headers fmt::fmt
    Boost::headers magic_enum::magic_enum)
target_include_directories(bbnet20 PRIVATE "${MINIUPNPC_INCLUDE}")

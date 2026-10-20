# lr_geoip — Plugify C++ port of the Pisex LR module "[LR] GEOIP"
# (the original LR_ExStatsGeoIP module, Metamod plugin lr_geoip).
#
#   lr_geoip_core       engine-free core (address extraction, the `_geoip` SQL) + the local MaxMind
#                       DB reader (src/mmdb.cpp; the build image has no libmaxminddb); also used by
#                       tests/geoip_tests.cpp against the shipped GeoLite2 databases.
#   plugify-lr_geoip    the plugin shared library (entry "bin/lr_geoip")

set(LR_GEOIP_VERSION "1.0.2")

# --- core ---------------------------------------------------------------------
add_library(lr_geoip_core STATIC src/geoip_core.cpp src/mmdb.cpp)
target_include_directories(lr_geoip_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# pisexcompat brings the KeyValues parser (and, PUBLIC-linked, the s2sdk/hudmenu globals);
# plgdb is the database layer.
target_link_libraries(lr_geoip_core PUBLIC plgdb pisexcompat)
target_compile_options(lr_geoip_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_geoip_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated levels_ranks bindings -------------------------------------------
# The __levels_ranks_* globals have to be compiled into every consumer plugin; plugins/levels_ranks
# builds them as the object library `lr_bindings` (it sorts before this directory), this is the
# fallback for a build of this directory alone.
if(NOT TARGET lr_bindings AND EXISTS
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    add_library(lr_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(lr_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(lr_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()

# --- plugin -------------------------------------------------------------------
add_library(plugify-lr_geoip SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:lr_bindings>
)
target_include_directories(plugify-lr_geoip PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated lr/plg/s2sdk headers and lr_api.hpp must not fail -Werror on
# our sources.
target_include_directories(plugify-lr_geoip SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include"
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
        "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-lr_geoip PRIVATE
        lr_geoip_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-lr_geoip PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-lr_geoip PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="lr_geoip"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-lr_geoip PROPERTIES OUTPUT_NAME lr_geoip)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-lr_geoip PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/lr_geoip.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lr_geoip.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Engine-free tests: the shipped GeoLite2 databases and the
# SQL against SQLite and the test MySQL of ./test.sh.
add_executable(lr_geoip_tests tests/geoip_tests.cpp)
target_link_libraries(lr_geoip_tests PRIVATE lr_geoip_core -static-libstdc++ -static-libgcc)
target_compile_options(lr_geoip_tests PRIVATE -Wall -Wextra -Werror)
set(LR_GEOIP_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist/configs/geoip" CACHE PATH
    "Directory with GeoLite2-City.mmdb and GeoLite2-Country.mmdb for the tests")
target_compile_definitions(lr_geoip_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        LR_GEOIP_TEST_DATA_DIR="${LR_GEOIP_TEST_DATA_DIR}")
set_target_properties(lr_geoip_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

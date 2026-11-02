# lr_geoip — Plugify C++ port of the Pisex LR module "[LR] GEOIP"
# (the original LR_ExStatsGeoIP module, Metamod plugin lr_geoip).
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no hudmenu and no guard library. The
# only engine reads are the client's address through s2sdk::GetClientIp (behind the map gate and
# the client checks of src/plugin.cpp) and the levels_ranks player-loaded hook; the database path
# is resolved by the plugin's own helpers in src/geoip_core.cpp.
#
#   lr_geoip_core       engine-free core (address extraction, the database path helpers, the
#                       `_geoip` SQL) + the local MaxMind DB reader (src/mmdb.cpp; the build image
#                       has no libmaxminddb); also used by tests/lr_geoip_tests.cpp against the
#                       shipped GeoLite2 databases.
#   plugify-lr_geoip    the plugin shared library (entry "bin/lr_geoip")

set(LR_GEOIP_VERSION "1.1.0")

# --- core ---------------------------------------------------------------------
add_library(lr_geoip_core STATIC src/geoip_core.cpp src/mmdb.cpp)
target_include_directories(lr_geoip_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# plgdb is the database layer; nothing else - the core has no s2sdk/plugify/pisexcompat dependency.
target_link_libraries(lr_geoip_core PUBLIC plgdb)
target_compile_options(lr_geoip_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(lr_geoip_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- generated levels_ranks bindings -------------------------------------------
# The __levels_ranks_* globals have to be compiled into every consumer plugin; plugins/levels_ranks
# builds them as the object library `lr_bindings` (it sorts before this directory), this is the
# fallback for a build of this directory alone (the exported repository carries the generated
# sources under plugins/levels_ranks/include).
if(NOT TARGET lr_bindings AND EXISTS
        "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    add_library(lr_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp")
    target_include_directories(lr_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/levels_ranks/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(lr_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
endif()
if(NOT TARGET lr_bindings)
    message(FATAL_ERROR "lr_geoip needs the generated levels_ranks consumer bindings "
            "(plugins/levels_ranks/include/external/plugify/include/levels_ranks/levels_ranks.cpp)")
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
        plugify_sdk
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
# Engine-free tests: the shipped GeoLite2 databases, the path helpers and the
# SQL against SQLite and the test MySQL of ./test.sh. Nothing here links plugify_sdk, lr_bindings,
# s2sdk or pisexcompat.
add_executable(lr_geoip_tests tests/lr_geoip_tests.cpp)
target_link_libraries(lr_geoip_tests PRIVATE lr_geoip_core -static-libstdc++ -static-libgcc)
target_compile_options(lr_geoip_tests PRIVATE -Wall -Wextra -Werror)
set(LR_GEOIP_TEST_DATA_DIR "" CACHE PATH
    "Directory with GeoLite2-City.mmdb and GeoLite2-Country.mmdb for the tests (auto-detected)")
# The databases are ~68 MB of third-party data that are not committed. An explicitly supplied
# directory (cache variable or LR_GEOIP_TEST_DATA_DIR environment variable) wins only when it holds
# BOTH files; otherwise the usual checkout and container locations are tried in order. This is only
# the configure-time hint - the test re-resolves the directory at runtime, so the same binary works
# with the data mounted later (e.g. at /ref). LR_GEOIP_TEST_HAS_DB=1 records that a complete
# City+Country pair existed at build time: if it is gone at runtime the test FAILS instead of
# hiding a broken data mount. Without a pair the GeoLite2 sections print an explicit SKIP, and an
# unreadable/corrupt database (when the pair is there) is always a real test failure.
set(_lr_geoip_data_candidates "")
if(LR_GEOIP_TEST_DATA_DIR)
    list(APPEND _lr_geoip_data_candidates "${LR_GEOIP_TEST_DATA_DIR}")
endif()
if(DEFINED ENV{LR_GEOIP_TEST_DATA_DIR} AND NOT "$ENV{LR_GEOIP_TEST_DATA_DIR}" STREQUAL "")
    list(APPEND _lr_geoip_data_candidates "$ENV{LR_GEOIP_TEST_DATA_DIR}")
endif()
list(APPEND _lr_geoip_data_candidates
        "${CMAKE_CURRENT_SOURCE_DIR}/dist/configs/geoip"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/geoip"
        "${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/configs/geoip"
        "${CMAKE_SOURCE_DIR}/ref/prod-27016/game/csgo/addons/configs/geoip"
        "/ref/prod-27016/addons/configs/geoip")
set(_lr_geoip_data_dir "")
foreach(candidate IN LISTS _lr_geoip_data_candidates)
    if(EXISTS "${candidate}/GeoLite2-City.mmdb" AND EXISTS "${candidate}/GeoLite2-Country.mmdb")
        set(_lr_geoip_data_dir "${candidate}")
        break()
    endif()
endforeach()
if(_lr_geoip_data_dir)
    set(LR_GEOIP_TEST_DATA_DIR "${_lr_geoip_data_dir}" CACHE PATH
        "Directory with GeoLite2-City.mmdb and GeoLite2-Country.mmdb for the tests" FORCE)
    set(LR_GEOIP_TEST_HAS_DB 1)
    message(STATUS "lr_geoip: GeoLite2 City+Country found in ${_lr_geoip_data_dir}")
else()
    set(LR_GEOIP_TEST_HAS_DB 0)
    message(STATUS "lr_geoip: no complete GeoLite2 City+Country pair found - "
            "the GeoLite2 sections will be skipped (set LR_GEOIP_TEST_DATA_DIR to the directory "
            "holding both .mmdb files)")
endif()
target_compile_definitions(lr_geoip_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        LR_GEOIP_TEST_HAS_DB=${LR_GEOIP_TEST_HAS_DB}
        LR_GEOIP_TEST_DATA_DIR="${LR_GEOIP_TEST_DATA_DIR}")
set_target_properties(lr_geoip_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

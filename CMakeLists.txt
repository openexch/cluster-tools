cmake_minimum_required(VERSION 3.16)
project(cluster_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The hexagon's build seam: domain + ports (and their tests) always build, with no
# aeron checkout, no java, no generated code. The transport side is opt-in.
option(WITH_AERON "Build the aeron adapter and the apps that need it" OFF)
set(AERON_SRC "" CACHE PATH
    "aeron source checkout (tag 1.52.2) — required when WITH_AERON=ON")
set(AERON_LIB "" CACHE FILEPATH
    "prebuilt libaeron_static.a (see poc/scripts/build-aeron.sh) — required when WITH_AERON=ON")
set(MATCH_SCHEMA "" CACHE FILEPATH
    "match order-schema.xml extracted from match main (git show origin/main:match-common/src/main/resources/sbe/order-schema.xml) — required when WITH_AERON=ON")
set(SBE_JAR "" CACHE FILEPATH
    "sbe-all fat jar for codec generation; downloaded into the build dir when empty")

include(CTest)

add_subdirectory(domain)
add_subdirectory(ports)
add_subdirectory(adapters/json)   # no transport dependency — builds and tests everywhere

if(WITH_AERON)
    if(NOT EXISTS "${AERON_SRC}/aeron-client/src/main/cpp_wrapper")
        message(FATAL_ERROR
            "WITH_AERON=ON needs -DAERON_SRC=<aeron checkout>; "
            "no cpp_wrapper under '${AERON_SRC}'")
    endif()
    if(NOT EXISTS "${AERON_LIB}")
        message(FATAL_ERROR
            "WITH_AERON=ON needs -DAERON_LIB=<path to libaeron_static.a>; "
            "'${AERON_LIB}' not found")
    endif()
    add_subdirectory(adapters/sbe)
    add_subdirectory(adapters/aeron)
    add_subdirectory(app/switch-behavior)
endif()

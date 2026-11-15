# A separate C++20 project that links the INSTALLED cluster_tools package, as another repository does. It never sees
# this source tree: only CMAKE_PREFIX_PATH (the install prefix). check-package.sh drives it.
#
#   CT_EXPECT_SOURCE_SHA       when set, the package must have been built from exactly this commit (the consumer's pin)
#   CONSUMER_EXTRA_COMPONENTS  more components to require (the control asks for one that does not exist)
cmake_minimum_required(VERSION 3.21)
project(cluster_tools_package_consumer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CT_EXPECT_SOURCE_SHA "" CACHE STRING "the commit the consumer pins; empty = not checked")
set(CONSUMER_EXTRA_COMPONENTS "" CACHE STRING "more components to require")

find_package(cluster_tools CONFIG REQUIRED COMPONENTS client_core runtime ${CONSUMER_EXTRA_COMPONENTS})
message(STATUS "cluster_tools package: source_sha=${cluster_tools_SOURCE_SHA} with_aeron=${cluster_tools_WITH_AERON} "
    "money_schema=${cluster_tools_MONEY_SCHEMA_VERSION}/${cluster_tools_MONEY_SCHEMA_SHA256} components=${cluster_tools_COMPONENTS_BUILT}")
if(NOT CT_EXPECT_SOURCE_SHA STREQUAL "" AND NOT cluster_tools_SOURCE_SHA STREQUAL CT_EXPECT_SOURCE_SHA)
    message(FATAL_ERROR "the cluster_tools package was built from ${cluster_tools_SOURCE_SHA}; the consumer pins ${CT_EXPECT_SOURCE_SHA}")
endif()

add_executable(pure_consumer pure_consumer.cpp)
target_link_libraries(pure_consumer PRIVATE cluster_tools::client_core cluster_tools::runtime)
target_compile_options(pure_consumer PRIVATE -Wall -Wextra -Werror)

if(cluster_tools_WITH_AERON)
    find_package(cluster_tools CONFIG REQUIRED COMPONENTS client client_aeron runtime_aeron runtime_embedded_driver sbe_codecs)
    add_executable(aeron_consumer aeron_consumer.cpp)
    # The embedded driver first: its archive resolves the C client symbols before aeron_static is scanned.
    target_link_libraries(aeron_consumer PRIVATE cluster_tools::runtime_embedded_driver cluster_tools::runtime_aeron
        cluster_tools::client_aeron cluster_tools::client)
    target_compile_options(aeron_consumer PRIVATE -Wall -Wextra)
endif()

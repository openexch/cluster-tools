# A separate C++20 project that links the archive follow library from the INSTALLED cluster_tools package, as
# another repository does (only CMAKE_PREFIX_PATH, never this source tree). check-archive-package.sh drives it.
#
#   CT_EXPECT_SOURCE_SHA     when set, the package must have been built from exactly this commit
#   REQUIRE_ARCHIVE_AERON    require archive_aeron even when the package does not list it (the control)
cmake_minimum_required(VERSION 3.21)
project(cluster_tools_archive_package_consumer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CT_EXPECT_SOURCE_SHA "" CACHE STRING "the commit the consumer pins; empty = not checked")
option(REQUIRE_ARCHIVE_AERON "require the archive_aeron component" OFF)

find_package(cluster_tools CONFIG REQUIRED COMPONENTS archive_core)
message(STATUS "cluster_tools package: source_sha=${cluster_tools_SOURCE_SHA} components=${cluster_tools_COMPONENTS_BUILT}")
if(NOT CT_EXPECT_SOURCE_SHA STREQUAL "" AND NOT cluster_tools_SOURCE_SHA STREQUAL CT_EXPECT_SOURCE_SHA)
    message(FATAL_ERROR "the cluster_tools package was built from ${cluster_tools_SOURCE_SHA}; the consumer pins ${CT_EXPECT_SOURCE_SHA}")
endif()

add_executable(archive_pure_consumer archive_pure_consumer.cpp)
target_link_libraries(archive_pure_consumer PRIVATE cluster_tools::archive_core)
target_compile_options(archive_pure_consumer PRIVATE -Wall -Wextra -Werror)

if(REQUIRE_ARCHIVE_AERON OR "archive_aeron" IN_LIST cluster_tools_COMPONENTS_BUILT)
    find_package(cluster_tools CONFIG REQUIRED COMPONENTS archive_aeron client_aeron runtime_embedded_driver)
    add_executable(archive_tail_consumer archive_tail_consumer.cpp)
    # The embedded driver first: its archive resolves the C client symbols before aeron_static is scanned.
    target_link_libraries(archive_tail_consumer PRIVATE cluster_tools::runtime_embedded_driver cluster_tools::archive_aeron
        cluster_tools::client_aeron)
    target_compile_options(archive_tail_consumer PRIVATE -Wall -Wextra)
endif()

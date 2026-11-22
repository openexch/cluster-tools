# A separate C++20 project that links what a market data service takes from the INSTALLED cluster_tools package: the
# forwarder domain (the book ladder), the forwarder codec (the 22 / 25 / 21 decode), the ME journal record 1 decode
# and the archive follow library's stall verdicts. Only CMAKE_PREFIX_PATH, never this source tree;
# check-marketdata-package.sh drives it.
#
#   CT_EXPECT_SOURCE_SHA   when set, the package must have been built from exactly this commit
cmake_minimum_required(VERSION 3.21)
project(cluster_tools_marketdata_package_consumer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CT_EXPECT_SOURCE_SHA "" CACHE STRING "the commit the consumer pins; empty = not checked")

find_package(cluster_tools CONFIG REQUIRED COMPONENTS forwarder_domain forwarder_codec journal_codec archive_core)
message(STATUS "cluster_tools package: source_sha=${cluster_tools_SOURCE_SHA} components=${cluster_tools_COMPONENTS_BUILT}")
if(NOT CT_EXPECT_SOURCE_SHA STREQUAL "" AND NOT cluster_tools_SOURCE_SHA STREQUAL CT_EXPECT_SOURCE_SHA)
    message(FATAL_ERROR "the cluster_tools package was built from ${cluster_tools_SOURCE_SHA}; the consumer pins ${CT_EXPECT_SOURCE_SHA}")
endif()

add_executable(marketdata_consumer marketdata_consumer.cpp)
# forwarder_codec brings forwarder_domain through its interface; the domain is not listed here on purpose, so a
# package whose codec does not carry it fails the link.
target_link_libraries(marketdata_consumer PRIVATE cluster_tools::forwarder_codec cluster_tools::journal_codec
    cluster_tools::archive_core)
target_compile_options(marketdata_consumer PRIVATE -Wall -Wextra -Werror)

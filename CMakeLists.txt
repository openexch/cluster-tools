cmake_minimum_required(VERSION 3.16)
project(cluster_tools LANGUAGES C CXX)

# C++20 for every runtime component (AGENTS.md); the domain-only build keeps the low
# cmake floor so the fast CI job runs on any runner, the Aeron build needs >= 3.30
# (Aeron 1.53.1's own floor) and checks it below.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The hexagon's build seam: domain + ports + the pure client parts (and their tests)
# always build, with no aeron checkout, no java, no generated code. Two opt-in layers sit on
# top: WITH_CODECS (Java only: SBE codegen for the money, order and ME-journal schemas, the
# EngineConfig codec and the codec goldens) and WITH_AERON (everything that links Aeron, which
# implies WITH_CODECS). Every pinned input of the opt-in layers is named in dependencies.lock
# and fetched + verified by scripts/fetch-deps.sh and scripts/fetch-schemas.sh.
option(WITH_CODECS "Generate the SBE codecs (needs java 21 + the pinned sbe-tool/agrona jars and the three wire schemas; no Aeron)" OFF)
option(WITH_AERON "Build the aeron adapter, the cluster client transport and the apps that need it (implies WITH_CODECS)" OFF)
set(AERON_TARBALL "" CACHE FILEPATH
    "Aeron source tarball at the dependencies.lock version and sha256 (scripts/fetch-deps.sh) - required when WITH_AERON=ON; configure verifies and extracts it")
set(SBE_CLASSPATH "" CACHE STRING
    "sbe-tool + agrona jars at the dependencies.lock versions, ':'-separated (scripts/fetch-deps.sh) - required when WITH_CODECS=ON or WITH_AERON=ON")
set(MONEY_SCHEMA "" CACHE FILEPATH
    "assets money-schema.xml (scripts/fetch-schemas.sh) - required when WITH_CODECS=ON or WITH_AERON=ON")
set(MATCH_SCHEMA "" CACHE FILEPATH
    "match order-schema.xml (scripts/fetch-schemas.sh) - required when WITH_CODECS=ON or WITH_AERON=ON")
set(ME_JOURNAL_SCHEMA "" CACHE FILEPATH
    "match me-journal-schema.xml, schemaId 4 (scripts/fetch-schemas.sh) - required when WITH_CODECS=ON or WITH_AERON=ON")
option(CT_BUILD_MEDIA_DRIVER "Build aeronmd from the verified Aeron tarball (integration tests and the published artifact need it)" ON)
option(CT_INTEGRATION_TESTS "Register the real-cluster integration test (java + CT_IT_ASSETS_JAR + CT_IT_MATCH_JAR, see client/it/run-it.sh)" OFF)

include(CTest)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock" CT_DEPENDENCY_LOCK)

add_subdirectory(domain)
add_subdirectory(ports)
add_subdirectory(adapters/json)   # no transport dependency - builds and tests everywhere
add_subdirectory(client)          # the pure parts build everywhere; the transport parts need WITH_AERON

if(WITH_AERON)
    set(WITH_CODECS ON)
endif()

if(WITH_CODECS)
    include(cmake/VerifiedDependencies.cmake)
    add_subdirectory(adapters/sbe)
endif()

if(WITH_AERON)
    add_subdirectory(client/aeron)
    add_subdirectory(adapters/aeron)
    add_subdirectory(app/switch-behavior)
    add_subdirectory(poc)
endif()

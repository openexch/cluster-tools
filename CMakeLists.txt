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
# RC2 S9a: the C++ settlement bridge's archive side (archive/aeron, ct-journal-tail) on the Aeron C
# archive client built from the verified tarball. Requires WITH_AERON; pins the money and journal
# schemas by EXACT sha256 (cmake/VerifiedDependencies.cmake).
option(WITH_BRIDGE "Build the Aeron C archive client, the bridge archive adapter and ct-journal-tail (requires WITH_AERON)" OFF)
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
# RC2 S9c: the bridge's negative-control and test-corruption binaries (ct_bridge_nc_*, ct_bridge_negctl, ct_bridge_tc_*,
# contract 12.4). Never published: publish.yml refuses them.
option(CT_BRIDGE_TEST_BUILDS "Build the never-published ct_bridge_nc_* / ct_bridge_negctl / ct_bridge_tc_* binaries (T1 drills)" OFF)
# RC2 S12: the forwarder's negative-control and test-corruption binaries (ct_forwarder_nc_*, ct_forwarder_tc_*,
# contract 11.4). Never published: publish.yml refuses a binary list that names them.
option(CT_FORWARDER_TEST_BUILDS "Build the never-published ct_forwarder_nc_* / ct_forwarder_tc_* binaries (T1 negative controls)" OFF)

include(CTest)

if(WITH_BRIDGE AND NOT WITH_AERON)
    message(FATAL_ERROR "WITH_BRIDGE=ON requires WITH_AERON=ON (the archive client links the verified Aeron tree)")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock" CT_DEPENDENCY_LOCK)

# The ME journal golden fixtures are verified in every configuration (RC2 S9a 9.2 FixtureGuard):
# the pure archive tests read them in the WITH_AERON=OFF job too.
include(cmake/VerifyMeJournalFixtures.cmake)
ct_verify_me_journal_fixtures("${CMAKE_CURRENT_SOURCE_DIR}/archive/tests/fixtures/mejournal"
    "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock")
file(GLOB CT_MEJOURNAL_FIXTURE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/archive/tests/fixtures/mejournal/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CT_MEJOURNAL_FIXTURE_FILES}) # an edited .bin re-runs the guard (review N-6)
# RC2 S9b W1: the E1 corpus written by the S9h harness (domain/bridge/tests/fixtures/e1) is verified in every
# configuration too: the pure bridge tests read it in the WITH_AERON=OFF job.
include(cmake/VerifyE1Fixtures.cmake)
ct_verify_e1_fixtures("${CMAKE_CURRENT_SOURCE_DIR}/domain/bridge/tests/fixtures/e1"
    "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock")
file(GLOB_RECURSE CT_E1_FIXTURE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/domain/bridge/tests/fixtures/e1/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CT_E1_FIXTURE_FILES})

# RC2 S16 W4: the binaries' source_sha, rendered as /health.build by ct-forwarder and ct-bridge. Named by -DCT_BUILD_SOURCE_SHA,
# else the git HEAD of this tree at configure time (re-configure after a commit), else "unknown". Never guessed: a malformed
# explicit value is an error (strict mode).
set(CT_BUILD_SOURCE_SHA "" CACHE STRING "40-hex commit of this tree compiled into ct-forwarder/ct-bridge (/health.build); empty = git HEAD at configure")
if(CT_BUILD_SOURCE_SHA STREQUAL "")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse HEAD
            OUTPUT_VARIABLE CT_GIT_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE CT_GIT_RC)
    endif()
    if(DEFINED CT_GIT_RC AND CT_GIT_RC EQUAL 0)
        set(CT_SOURCE_SHA_EFFECTIVE "${CT_GIT_HEAD}")
    else()
        set(CT_SOURCE_SHA_EFFECTIVE "unknown")
    endif()
else()
    set(CT_SOURCE_SHA_EFFECTIVE "${CT_BUILD_SOURCE_SHA}")
endif()
if(NOT CT_SOURCE_SHA_EFFECTIVE MATCHES "^([0-9a-f]+|unknown)$")
    message(FATAL_ERROR "CT_BUILD_SOURCE_SHA '${CT_SOURCE_SHA_EFFECTIVE}' is neither lowercase hex nor 'unknown'")
endif()
message(STATUS "source_sha (/health.build): ${CT_SOURCE_SHA_EFFECTIVE}")

add_subdirectory(domain)
add_subdirectory(domain/forwarder) # RC2 S12: the forwarder's pure core (every configuration)
add_subdirectory(ports)
add_subdirectory(adapters/json)   # no transport dependency - builds and tests everywhere
add_subdirectory(client)          # the pure parts build everywhere; the transport parts need WITH_AERON
add_subdirectory(archive)         # pure archive core everywhere; archive/aeron with WITH_BRIDGE
add_subdirectory(domain/bridge)   # RC2 S9b: the bridge domain (pure, every configuration; needs ct_archive_core)
find_package(Threads REQUIRED)

if(WITH_AERON)
    set(WITH_CODECS ON)
endif()

# RC2 S9h 9.2 / S9b W1: a feed harness jar given to any build must carry the lock's money schema and the pinned core's
# production tree, and hash as the lock pins. It is MANDATORY under WITH_BRIDGE (the bridge's translation and
# real-core graders, S9b 13.11-13.17, take their expected bytes only from it); a WITH_CODECS build without it skips
# the differential and oracle tests (they are listed as not built in the configure log).
if(DEFINED CT_FEED_HARNESS_JAR AND NOT CT_FEED_HARNESS_JAR STREQUAL "")
    include(cmake/VerifyFeedHarness.cmake)
    ct_verify_feed_harness("${CT_FEED_HARNESS_JAR}" "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock"
        "${CMAKE_CURRENT_BINARY_DIR}/ct-feed-harness-guard")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CT_FEED_HARNESS_JAR}")
    set(CT_HAVE_FEED_HARNESS ON)
elseif(WITH_BRIDGE)
    message(FATAL_ERROR "WITH_BRIDGE=ON: CT_FEED_HARNESS_JAR is required (the S9h harness jar pinned by "
        "feed_harness_sha256; scripts/fetch-deps.sh <cache> --harness prints the argument)")
else()
    set(CT_HAVE_FEED_HARNESS OFF)
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
add_subdirectory(app/common)    # RC2 S9c W1: the process machinery both C++ processes share (pure; ConductorWatch with WITH_AERON)
add_subdirectory(app/forwarder) # RC2 S12: pure parts everywhere, ct-forwarder and its agent tests with WITH_AERON

if(WITH_BRIDGE)
    include(cmake/AeronArchive.cmake)
    add_subdirectory(archive/aeron)
    add_subdirectory(app/journal-tail)
endif()
add_subdirectory(app/bridge) # RC2 S9c: ct_bridge_pure everywhere; ct-bridge and its app tests with WITH_AERON AND WITH_BRIDGE

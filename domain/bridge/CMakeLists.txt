# domain/bridge - the C++ settlement bridge's domain (RC2 S9b contract sections 2-12): staging, the in-flight ring
# and key table, the cursor and sync, epoch binding, the per-source verdicts and rotation, the halt machine, the
# fence and storm guard, the metrics it owns. No Aeron, no SBE, no I/O: builds and tests in every configuration,
# the WITH_AERON=OFF job included. It plugs into S9a's frozen archive interfaces (ct_archive_core) and nothing else.
#
# ct_bridge_domain         the production library: every CT_TEST_DISABLE_* seam compiles to nothing.
# ct_bridge_domain_seamed  the same sources with CT_TEST_SEAMS, linked ONLY by test targets.

set(CT_BRIDGE_DOMAIN_SOURCES
    src/BridgeDomain.cpp
    src/HaltMachine.cpp
    src/InflightRing.cpp
    src/Stager.cpp
    src/Sync.cpp
    src/TestSeams.cpp
    src/Types.cpp
    src/Verdicts.cpp)

add_library(ct_bridge_domain STATIC ${CT_BRIDGE_DOMAIN_SOURCES})
add_library(cluster_tools::bridge_domain ALIAS ct_bridge_domain)
target_include_directories(ct_bridge_domain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ct_bridge_domain PUBLIC ct_archive_core)
target_compile_options(ct_bridge_domain PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_library(ct_bridge_domain_seamed STATIC ${CT_BRIDGE_DOMAIN_SOURCES})
    target_include_directories(ct_bridge_domain_seamed PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(ct_bridge_domain_seamed PUBLIC CT_TEST_SEAMS=1)
    # the seamed archive core: the property test drives the real follower and may switch its rules off too
    target_link_libraries(ct_bridge_domain_seamed PUBLIC ct_archive_core_seamed)
    target_compile_options(ct_bridge_domain_seamed PRIVATE -Wall -Wextra -Wpedantic)

    set(CT_BRIDGE_TEST_SOURCES
        tests/TestMain.cpp
        tests/CursorTableTest.cpp
        tests/StagingTest.cpp
        tests/RingTest.cpp
        tests/ReportRuleTest.cpp
        tests/RotationTest.cpp
        tests/BehindTest.cpp
        tests/HaltMachineTest.cpp
        tests/StormGuardTest.cpp
        tests/FenceTest.cpp
        tests/MetricsTest.cpp
        tests/E1GateTest.cpp
        tests/LaggedEgressTest.cpp
        tests/BridgeDomainPropertyTest.cpp)
    add_executable(ct_bridge_domain_tests ${CT_BRIDGE_TEST_SOURCES})
    target_link_libraries(ct_bridge_domain_tests PRIVATE ct_bridge_domain_seamed)
    target_include_directories(ct_bridge_domain_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${PROJECT_SOURCE_DIR}/archive/tests")
    target_compile_definitions(ct_bridge_domain_tests PRIVATE
        CT_E1_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/e1"
        CT_MEJOURNAL_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/archive/tests/fixtures/mejournal")
    target_compile_options(ct_bridge_domain_tests PRIVATE -Wall -Wextra)
    add_test(NAME bridge.domain COMMAND ct_bridge_domain_tests)
    set_tests_properties(bridge.domain PROPERTIES TIMEOUT 1800 ENVIRONMENT "CT_BRIDGE_PROPERTY_RUNS=2000")

    # Section 4 "allocates nothing after construction" (review NOTE-10): a counted-allocation window (own binary:
    # it replaces the global operator new)
    add_executable(ct_bridge_alloc_tests tests/ZeroAllocTest.cpp)
    target_link_libraries(ct_bridge_alloc_tests PRIVATE ct_bridge_domain_seamed)
    target_include_directories(ct_bridge_alloc_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${PROJECT_SOURCE_DIR}/archive/tests")
    target_compile_definitions(ct_bridge_alloc_tests PRIVATE
        CT_MEJOURNAL_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/archive/tests/fixtures/mejournal")
    target_compile_options(ct_bridge_alloc_tests PRIVATE -Wall -Wextra)
    add_test(NAME bridge.zero_alloc COMMAND ct_bridge_alloc_tests)

    # The RED column (light method): every (test, rule) pair of negative-controls.txt must FAIL with the rule
    # switched off, for the stated reason, and PASS with it on (archive/tests/run-negative-controls.sh).
    add_test(NAME bridge.domain_negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_bridge_domain_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls.txt")
    set_tests_properties(bridge.domain_negative_controls PROPERTIES TIMEOUT 3600
        ENVIRONMENT "CT_BRIDGE_PROPERTY_RUNS=400")

    # W1: the E1 fixture guard refuses an edited fixture, an unlisted file and a lock without the pin.
    add_test(NAME bridge.e1_fixture_guard
        COMMAND ${CMAKE_COMMAND} -DCT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DCT_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/e1-fixture-guard
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/e1_fixture_guard_test.cmake")
    set_tests_properties(bridge.e1_fixture_guard PROPERTIES TIMEOUT 120)

    # RC2 S10' 3.4 (lane C, W2b): TwoLaneDomainPropertyTest, two real BridgeDomain lanes over one stub core written to
    # the 4R rule (per-lane halt table, overflow record, holder set). Its own binary and RED table, so the domain's
    # suite and negative-controls.txt stay untouched; the stub-core seams are test-local CT_TEST_DISABLE_* variables.
    add_executable(ct_bridge_twolane_tests tests/TestMain.cpp tests/TwoLaneDomainPropertyTest.cpp)
    target_link_libraries(ct_bridge_twolane_tests PRIVATE ct_bridge_domain_seamed)
    target_include_directories(ct_bridge_twolane_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests" "${PROJECT_SOURCE_DIR}/archive/tests")
    target_compile_definitions(ct_bridge_twolane_tests PRIVATE
        CT_E1_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/e1"
        CT_MEJOURNAL_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/archive/tests/fixtures/mejournal")
    target_compile_options(ct_bridge_twolane_tests PRIVATE -Wall -Wextra)
    add_test(NAME bridge.twolane COMMAND ct_bridge_twolane_tests)
    set_tests_properties(bridge.twolane PROPERTIES TIMEOUT 1800 ENVIRONMENT "CT_BRIDGE_TWOLANE_RUNS=1000")
    add_test(NAME bridge.twolane_negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_bridge_twolane_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls-twolane.txt")
    set_tests_properties(bridge.twolane_negative_controls PROPERTIES TIMEOUT 3600
        ENVIRONMENT "CT_BRIDGE_TWOLANE_RUNS=400")
endif()

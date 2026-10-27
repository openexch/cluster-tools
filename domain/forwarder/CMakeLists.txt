# domain/forwarder - the forwarder's pure core (RC2 S12 contract section 2): classification and the
# INVALID carrier, the send ring, the resync machine, the epoch binder, the admission watcher. No Aeron,
# no SBE, no I/O: builds and tests in every configuration, the WITH_AERON=OFF job included.
#
# ct_forwarder_domain         the production library: every CT_TEST_DISABLE_* seam compiles to nothing.
# ct_forwarder_domain_seamed  the same sources with CT_TEST_SEAMS, linked ONLY by test targets and the
#                             never-published ct_forwarder_nc_* / ct_forwarder_tc_* binaries.

set(CT_FORWARDER_DOMAIN_SOURCES
    src/DispatchRules.cpp
    src/EpochBinder.cpp
    src/ResyncMachine.cpp
    src/SendRing.cpp
    src/TestSeams.cpp)

add_library(ct_forwarder_domain STATIC ${CT_FORWARDER_DOMAIN_SOURCES})
add_library(cluster_tools::forwarder_domain ALIAS ct_forwarder_domain)
target_include_directories(ct_forwarder_domain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_options(ct_forwarder_domain PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING OR CT_FORWARDER_TEST_BUILDS)
    add_library(ct_forwarder_domain_seamed STATIC ${CT_FORWARDER_DOMAIN_SOURCES})
    target_include_directories(ct_forwarder_domain_seamed PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(ct_forwarder_domain_seamed PUBLIC CT_TEST_SEAMS=1)
    target_compile_options(ct_forwarder_domain_seamed PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(ct_forwarder_domain_tests
        tests/TestMain.cpp
        tests/DispatchRulesTest.cpp
        tests/CarrierTest.cpp
        tests/MappingTest.cpp
        tests/SendRingTest.cpp
        tests/EpochBinderTest.cpp
        tests/AdmissionWatcherTest.cpp
        tests/ResyncMachineTest.cpp
        tests/ResyncPropertyTest.cpp)
    target_link_libraries(ct_forwarder_domain_tests PRIVATE ct_forwarder_domain_seamed)
    target_include_directories(ct_forwarder_domain_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_options(ct_forwarder_domain_tests PRIVATE -Wall -Wextra)
    add_test(NAME forwarder.domain COMMAND ct_forwarder_domain_tests)

    # The RED column (light method): every (test, rule) pair of negative-controls.txt must FAIL with the
    # rule switched off, for the stated reason, and PASS with it on (archive/tests/run-negative-controls.sh).
    add_test(NAME forwarder.domain_negative_controls
        COMMAND bash "${PROJECT_SOURCE_DIR}/archive/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_forwarder_domain_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls.txt")
    set_tests_properties(forwarder.domain_negative_controls PROPERTIES TIMEOUT 900
        ENVIRONMENT "CT_FORWARDER_PROPERTY_SEEDS=2000")
endif()

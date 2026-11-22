# archive - the C++ bridge's archive side. The pure core (frame admission gate, chain
# model, follow decision table and loop, per-source resume memo, strict settings, metrics) builds
# in every configuration, the WITH_AERON=OFF job included; the Aeron adapter is archive/aeron
# (WITH_BRIDGE).
#
# ct_archive_core         the production library: the CT_TEST_DISABLE_* seams compile to nothing.
# ct_archive_core_seamed  the same sources with CT_TEST_SEAMS, linked ONLY by test targets: each rule
#                         the tests pin can be switched off (env CT_TEST_DISABLE_<RULE>=1) so the
#                         negative-control run proves the test fails without it.

set(CT_ARCHIVE_CORE_SOURCES
    src/ArchiveMetrics.cpp
    src/ArchiveSettings.cpp
    src/Chain.cpp
    src/FollowPolicy.cpp
    src/FramePeek.cpp
    src/ResumeMemo.cpp
    src/TestSeams.cpp)

add_library(ct_archive_core STATIC ${CT_ARCHIVE_CORE_SOURCES})
add_library(cluster_tools::archive_core ALIAS ct_archive_core)
set_target_properties(ct_archive_core PROPERTIES EXPORT_NAME archive_core)
target_include_directories(ct_archive_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_features(ct_archive_core PUBLIC cxx_std_20)
target_compile_options(ct_archive_core PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_library(ct_archive_core_seamed STATIC ${CT_ARCHIVE_CORE_SOURCES})
    target_include_directories(ct_archive_core_seamed PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(ct_archive_core_seamed PUBLIC CT_TEST_SEAMS=1)
    target_compile_options(ct_archive_core_seamed PRIVATE -Wall -Wextra -Wpedantic)

    add_executable(ct_archive_tests
        tests/TestMain.cpp
        tests/FramePeekTest.cpp
        tests/ChainModelTest.cpp
        tests/ResumeMemoTest.cpp
        tests/FollowPolicyTest.cpp
        tests/StallVerdictsTest.cpp
        tests/ChainWalkPropertyTest.cpp
        tests/ArchiveSettingsTest.cpp)
    target_link_libraries(ct_archive_tests PRIVATE ct_archive_core_seamed)
    target_include_directories(ct_archive_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(ct_archive_tests PRIVATE
        CT_MEJOURNAL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mejournal")
    target_compile_options(ct_archive_tests PRIVATE -Wall -Wextra)
    add_test(NAME archive.pure COMMAND ct_archive_tests)

    # The RED column of 9.1: every (test, rule) pair of negative-controls.txt must FAIL with the rule
    # switched off, and the same test must pass with it on.
    add_test(NAME archive.negative_controls
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_archive_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/negative-controls.txt")
    set_tests_properties(archive.negative_controls PROPERTIES TIMEOUT 900)

    # The follow library on the core order journal profile (a separate binary and table, so the bridge's
    # ct_archive_tests and negative-controls.txt stay byte for byte what they were).
    add_executable(ct_archive_profile_tests tests/TestMain.cpp tests/JournalProfileTest.cpp)
    target_link_libraries(ct_archive_profile_tests PRIVATE ct_archive_core_seamed)
    target_include_directories(ct_archive_profile_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(ct_archive_profile_tests PRIVATE
        CT_MEJOURNAL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mejournal")
    target_compile_options(ct_archive_profile_tests PRIVATE -Wall -Wextra)
    add_test(NAME archive.profile COMMAND ct_archive_profile_tests)
    add_test(NAME archive.profile_negative_controls
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-negative-controls.sh" "$<TARGET_FILE:ct_archive_profile_tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/journal-profile-negative-controls.txt")
    set_tests_properties(archive.profile_negative_controls PROPERTIES TIMEOUT 900)

    # 9.2 FixtureGuard: one edited byte, a regenerated dump without a lock bump, a placeholder pin.
    add_test(NAME archive.fixture_guard
        COMMAND ${CMAKE_COMMAND} -DCT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DCT_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/fixture-guard
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture_guard_test.cmake")
    set_tests_properties(archive.fixture_guard PROPERTIES TIMEOUT 120)
endif()

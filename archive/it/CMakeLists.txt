# archive/it (WITH_BRIDGE) - the 9.3 archive integration scenarios against a real Java Aeron Archive
# (JournalArchiveFixture.java on the pinned aeron-all jar). Built with WITH_BRIDGE; registered as a
# ctest only with CT_INTEGRATION_TESTS=ON and CT_IT_AERON_ALL_JAR (sha256 = the lock's
# aeron_all_sha256, verified here and again by run-it.sh), so the default test run stays hermetic.
add_executable(ct_archive_it ArchiveIntegration.cpp)
target_link_libraries(ct_archive_it PRIVATE ct_archive_aeron_seamed)
target_compile_options(ct_archive_it PRIVATE -Wall -Wextra)

set(CT_IT_AERON_ALL_JAR "" CACHE FILEPATH "aeron-all jar at the lock's aeron_all_version (archive integration fixture)")
if(CT_INTEGRATION_TESTS)
    if(NOT EXISTS "${CT_IT_AERON_ALL_JAR}")
        message(FATAL_ERROR "CT_INTEGRATION_TESTS=ON with WITH_BRIDGE=ON needs -DCT_IT_AERON_ALL_JAR "
            "(scripts/fetch-deps.sh <cache> --it)")
    endif()
    string(JSON CT_LOCK_AERON_ALL_SHA GET "${CT_DEPENDENCY_LOCK}" aeron_all_sha256)
    file(SHA256 "${CT_IT_AERON_ALL_JAR}" CT_IT_AERON_ALL_GOT)
    if(NOT CT_IT_AERON_ALL_GOT STREQUAL CT_LOCK_AERON_ALL_SHA)
        message(FATAL_ERROR "CT_IT_AERON_ALL_JAR ${CT_IT_AERON_ALL_JAR} has sha256 ${CT_IT_AERON_ALL_GOT}; "
            "dependencies.lock pins ${CT_LOCK_AERON_ALL_SHA}")
    endif()
    add_test(NAME archive.integration
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/run-it.sh" "${CMAKE_BINARY_DIR}" "${CT_IT_AERON_ALL_JAR}")
    set_tests_properties(archive.integration PROPERTIES TIMEOUT 3600 RUN_SERIAL TRUE)
endif()

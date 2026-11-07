# app/journal-tail (WITH_BRIDGE): ct-journal-tail, the archive side driven end to end without a core
# session (RC2 S9a section 0 item 4). Reads only; never writes a file.
add_executable(ct-journal-tail main.cpp)
target_link_libraries(ct-journal-tail PRIVATE ct_archive_aeron)
target_compile_options(ct-journal-tail PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    # The test build of the same main: the CT_TEST_DISABLE_TAIL_TERMINATE_HANDLER seam and the
    # CT_TEST_THROW_IN_SINK injection exist only here (9.3 journalTailExitsSeventyOnUncaughtException).
    add_executable(ct-journal-tail-seamed main.cpp)
    target_link_libraries(ct-journal-tail-seamed PRIVATE ct_archive_aeron_seamed)
    target_compile_options(ct-journal-tail-seamed PRIVATE -Wall -Wextra)
endif()

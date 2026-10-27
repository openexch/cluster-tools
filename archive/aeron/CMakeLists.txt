# archive/aeron (WITH_BRIDGE) - the archive side of the C++ bridge on the Aeron C archive client:
# ArchiveSource (one endpoint), ChainFollower (FollowPolicy wired to it), SourceSet (ordered sources,
# one attached at a time), and the static-assert translation unit pinning the pure 4.4 constants
# to the generated schema 4 codecs.
#
# ct_archive_aeron        production (links ct_archive_core: no test seam exists)
# ct_archive_aeron_seamed test targets only (links ct_archive_core_seamed, CT_TEST_SEAMS)

set(CT_ARCHIVE_AERON_SOURCES src/ArchiveSource.cpp src/SourceSet.cpp src/JournalCodecLayout.cpp)

add_library(ct_archive_aeron STATIC ${CT_ARCHIVE_AERON_SOURCES})
add_library(cluster_tools::archive_aeron ALIAS ct_archive_aeron)
target_include_directories(ct_archive_aeron PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ct_archive_aeron
    PUBLIC ct_archive_core ct_aeron_archive_client ct_client_aeron
    PRIVATE ct_sbe_codecs)
add_dependencies(ct_archive_aeron ct_sbe_generate)
target_compile_options(ct_archive_aeron PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    add_library(ct_archive_aeron_seamed STATIC ${CT_ARCHIVE_AERON_SOURCES})
    target_include_directories(ct_archive_aeron_seamed PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(ct_archive_aeron_seamed
        PUBLIC ct_archive_core_seamed ct_aeron_archive_client ct_client_aeron
        PRIVATE ct_sbe_codecs)
    add_dependencies(ct_archive_aeron_seamed ct_sbe_generate)
    target_compile_options(ct_archive_aeron_seamed PRIVATE -Wall -Wextra)

    add_executable(ct_archive_aeron_tests tests/ArchiveSourceTest.cpp)
    target_link_libraries(ct_archive_aeron_tests PRIVATE ct_archive_aeron_seamed)
    target_include_directories(ct_archive_aeron_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_archive_aeron_tests PRIVATE -Wall -Wextra)
    add_test(NAME archive.aeron_source COMMAND ct_archive_aeron_tests)

    add_executable(ct_journal_codec_layout_tests tests/JournalCodecLayoutTest.cpp)
    target_link_libraries(ct_journal_codec_layout_tests PRIVATE ct_archive_core ct_sbe_codecs)
    add_dependencies(ct_journal_codec_layout_tests ct_sbe_generate)
    target_include_directories(ct_journal_codec_layout_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_definitions(ct_journal_codec_layout_tests PRIVATE
        CT_MEJOURNAL_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/archive/tests/fixtures/mejournal")
    target_compile_options(ct_journal_codec_layout_tests PRIVATE -Wall -Wextra)
    add_test(NAME archive.journal_codec_layout COMMAND ct_journal_codec_layout_tests)
endif()

add_subdirectory(../it it)

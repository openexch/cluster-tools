# ct_aeron_archive_client (WITH_BRIDGE): the Aeron C archive client + its cpp_wrapper, built from
# the tree configure extracted from the verified tarball (cmake/VerifiedDependencies.cmake), never
# from a prebuilt library. Port of the execution projector's recipe
# (oms/execution-projector/CMakeLists.txt:42-64): the archive control codecs are generated as C
# with the PINNED sbe-tool (the hashed copies, re-hashed by gen-codecs.sh before every run) under
# the namespace aeron.archive.client, the C client sources compile into one static library, and
# the cpp_wrapper headers go on the include path. BUILD_AERON_ARCHIVE_API stays OFF: Aeron's own
# archive build needs Gradle.

set(CT_ARCHIVE_SRC "${AERON_SRC}/aeron-archive/src/main")
set(CT_ARCHIVE_CODEC_SCHEMA "${CT_ARCHIVE_SRC}/resources/archive/aeron-archive-codecs.xml")
foreach(required
        "${CT_ARCHIVE_SRC}/c/client/aeron_archive.h"
        "${CT_ARCHIVE_CODEC_SCHEMA}"
        "${CT_ARCHIVE_SRC}/cpp_wrapper/client/archive/AeronArchive.h")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "WITH_BRIDGE=ON: ${required} is missing in the extracted Aeron ${CT_AERON_VERSION} tree "
            "(the archive C client sources must come from the verified tarball)")
    endif()
endforeach()

set(CT_ARCHIVE_GEN_DIR "${CMAKE_BINARY_DIR}/generated/archive")
set(CT_ARCHIVE_GEN_STAMP "${CMAKE_BINARY_DIR}/generated/archive.stamp")
add_custom_command(
    OUTPUT "${CT_ARCHIVE_GEN_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CT_ARCHIVE_GEN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env
        "JAVA=${Java_JAVA_EXECUTABLE}" "SBE_CLASSPATH=${CT_SBE_VERIFIED_CLASSPATH}"
        "SBE_JAR_SHA256S=${CT_SBE_JAR_SHA256S}"
        bash "${PROJECT_SOURCE_DIR}/adapters/sbe/gen-codecs.sh" "${CT_ARCHIVE_GEN_DIR}/c" "${CT_ARCHIVE_GEN_STAMP}"
            "${CT_ARCHIVE_CODEC_SCHEMA}" C aeron.archive.client
    DEPENDS "${CT_ARCHIVE_CODEC_SCHEMA}" "${PROJECT_SOURCE_DIR}/adapters/sbe/gen-codecs.sh" ${CT_SBE_VERIFIED_JARS}
    COMMENT "sbe ${CT_LOCK_SBE_VERSION}: generating the Aeron archive C control codecs"
    VERBATIM)
add_custom_target(ct_aeron_archive_codecs DEPENDS "${CT_ARCHIVE_GEN_STAMP}")

file(GLOB CT_ARCHIVE_C_SOURCES CONFIGURE_DEPENDS "${CT_ARCHIVE_SRC}/c/client/*.c")
add_library(ct_aeron_archive_client STATIC ${CT_ARCHIVE_C_SOURCES})
add_dependencies(ct_aeron_archive_client ct_aeron_archive_codecs)
target_include_directories(ct_aeron_archive_client SYSTEM
    PUBLIC "${CT_ARCHIVE_SRC}/c" "${CT_ARCHIVE_SRC}/cpp_wrapper"
    PRIVATE "${CT_ARCHIVE_GEN_DIR}")
target_link_libraries(ct_aeron_archive_client PUBLIC aeron_static aeron_client_wrapper Threads::Threads)
string(REPLACE "." ";" CT_AERON_VERSION_PARTS "${CT_AERON_VERSION}")
list(GET CT_AERON_VERSION_PARTS 0 CT_AERON_VERSION_MAJOR)
list(GET CT_AERON_VERSION_PARTS 1 CT_AERON_VERSION_MINOR)
list(GET CT_AERON_VERSION_PARTS 2 CT_AERON_VERSION_PATCH)
target_compile_definitions(ct_aeron_archive_client PRIVATE
    AERON_VERSION_TXT="${CT_AERON_VERSION}" AERON_VERSION_MAJOR=${CT_AERON_VERSION_MAJOR}
    AERON_VERSION_MINOR=${CT_AERON_VERSION_MINOR} AERON_VERSION_PATCH=${CT_AERON_VERSION_PATCH}
    AERON_VERSION_GITSHA="tarball-${CT_LOCK_AERON_SHA}")
add_library(cluster_tools::aeron_archive_client ALIAS ct_aeron_archive_client)
message(STATUS "Aeron archive C client: ${CT_ARCHIVE_SRC}/c/client (codecs generated with sbe ${CT_LOCK_SBE_VERSION})")

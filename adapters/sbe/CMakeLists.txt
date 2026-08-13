# adapters/sbe — codec generation wiring. No generated code is committed: the cluster
# session protocol codecs are generated from the schema inside the aeron checkout into
# the build tree, and ct_sbe_codecs exposes only an include path.

find_package(Java 11 COMPONENTS Runtime REQUIRED)

set(CT_SBE_GEN_DIR "${CMAKE_BINARY_DIR}/generated/sbe")
set(CT_CLUSTER_SCHEMA
    "${AERON_SRC}/aeron-cluster/src/main/resources/cluster/aeron-cluster-codecs.xml")

if(NOT EXISTS "${CT_CLUSTER_SCHEMA}")
    message(FATAL_ERROR "cluster codec schema not found: ${CT_CLUSTER_SCHEMA}")
endif()

add_custom_command(
    OUTPUT "${CT_SBE_GEN_DIR}/io_aeron_cluster_codecs/MessageHeader.h"
    COMMAND ${CMAKE_COMMAND} -E env
        "JAVA=${Java_JAVA_EXECUTABLE}" "SBE_JAR=${SBE_JAR}"
        bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-codecs.sh"
        "${CT_SBE_GEN_DIR}" "${CT_CLUSTER_SCHEMA}"
    DEPENDS "${CT_CLUSTER_SCHEMA}" "${CMAKE_CURRENT_SOURCE_DIR}/gen-codecs.sh"
    COMMENT "sbe: generating C++ codecs for the aeron cluster session protocol"
    VERBATIM)

add_custom_target(ct_sbe_generate
    DEPENDS "${CT_SBE_GEN_DIR}/io_aeron_cluster_codecs/MessageHeader.h")

add_library(ct_sbe_codecs INTERFACE)
add_library(cluster_tools::sbe_codecs ALIAS ct_sbe_codecs)
add_dependencies(ct_sbe_codecs ct_sbe_generate)
target_include_directories(ct_sbe_codecs SYSTEM INTERFACE "${CT_SBE_GEN_DIR}")

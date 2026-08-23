# adapters/sbe — codec generation wiring plus the one compiled codec adapter. No
# generated code is committed: the cluster session protocol codecs (from the schema
# inside the aeron checkout) and the match order-schema codecs (from MATCH_SCHEMA, an
# operator-provided extract of match main — see the root CMakeLists) are generated into
# the build tree. ct_sbe_codecs exposes the include path; ct_adapter_sbe compiles the
# EngineConfig encoder against it so app code never includes a codec header.

find_package(Java 11 COMPONENTS Runtime REQUIRED)

set(CT_SBE_GEN_DIR "${CMAKE_BINARY_DIR}/generated/sbe")
set(CT_CLUSTER_SCHEMA
    "${AERON_SRC}/aeron-cluster/src/main/resources/cluster/aeron-cluster-codecs.xml")

if(NOT EXISTS "${CT_CLUSTER_SCHEMA}")
    message(FATAL_ERROR "cluster codec schema not found: ${CT_CLUSTER_SCHEMA}")
endif()
if(NOT EXISTS "${MATCH_SCHEMA}")
    message(FATAL_ERROR
        "match order schema not found: '${MATCH_SCHEMA}'. Point -DMATCH_SCHEMA at an "
        "extract of match main, e.g.\n"
        "  git -C <match-checkout> show origin/main:match-common/src/main/resources/sbe/order-schema.xml > order-schema.xml")
endif()

add_custom_command(
    OUTPUT
        "${CT_SBE_GEN_DIR}/io_aeron_cluster_codecs/MessageHeader.h"
        "${CT_SBE_GEN_DIR}/com_match_infrastructure_generated/EngineConfig.h"
    COMMAND ${CMAKE_COMMAND} -E env
        "JAVA=${Java_JAVA_EXECUTABLE}" "SBE_JAR=${SBE_JAR}"
        bash "${CMAKE_CURRENT_SOURCE_DIR}/gen-codecs.sh"
        "${CT_SBE_GEN_DIR}" "${CT_CLUSTER_SCHEMA}" "${MATCH_SCHEMA}"
    DEPENDS "${CT_CLUSTER_SCHEMA}" "${MATCH_SCHEMA}" "${CMAKE_CURRENT_SOURCE_DIR}/gen-codecs.sh"
    COMMENT "sbe: generating C++ codecs (aeron cluster session protocol + match order schema)"
    VERBATIM)

add_custom_target(ct_sbe_generate
    DEPENDS
        "${CT_SBE_GEN_DIR}/io_aeron_cluster_codecs/MessageHeader.h"
        "${CT_SBE_GEN_DIR}/com_match_infrastructure_generated/EngineConfig.h")

add_library(ct_sbe_codecs INTERFACE)
add_library(cluster_tools::sbe_codecs ALIAS ct_sbe_codecs)
add_dependencies(ct_sbe_codecs ct_sbe_generate)
target_include_directories(ct_sbe_codecs SYSTEM INTERFACE "${CT_SBE_GEN_DIR}")

add_library(ct_adapter_sbe STATIC
    src/EngineConfigCodec.cpp)
add_library(cluster_tools::adapter_sbe ALIAS ct_adapter_sbe)
add_dependencies(ct_adapter_sbe ct_sbe_generate)
target_include_directories(ct_adapter_sbe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ct_adapter_sbe
    PUBLIC ct_domain ct_ports
    PRIVATE ct_sbe_codecs)
target_compile_options(ct_adapter_sbe PRIVATE -Wall -Wextra)

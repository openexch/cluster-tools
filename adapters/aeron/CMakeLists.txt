# adapters/aeron — the one place in the repo that speaks aeron. Compiles against the
# cpp_wrapper headers from AERON_SRC; the prebuilt AERON_LIB (and its platform deps)
# is propagated to whatever links this adapter.

find_package(Threads REQUIRED)

add_library(ct_adapter_aeron STATIC
    src/AeronClusterSession.cpp)
add_library(cluster_tools::adapter_aeron ALIAS ct_adapter_aeron)

add_dependencies(ct_adapter_aeron ct_sbe_generate)

target_include_directories(ct_adapter_aeron PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(ct_adapter_aeron SYSTEM PRIVATE
    "${AERON_SRC}/aeron-client/src/main/cpp_wrapper"
    "${AERON_SRC}/aeron-client/src/main/c")

target_link_libraries(ct_adapter_aeron
    PUBLIC ct_ports
    PRIVATE ct_sbe_codecs
    INTERFACE "${AERON_LIB}" Threads::Threads ${CMAKE_DL_LIBS} m)

target_compile_options(ct_adapter_aeron PRIVATE -Wall -Wextra)

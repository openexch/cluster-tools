# adapters/aeron - the cold-path AeronClusterSession behind the virtual ClusterSession port
# (switch-behavior). Compiles against the cpp_wrapper of the Aeron tree extracted from the verified tarball and links the
# aeron C client built from that same tree (cmake/VerifiedDependencies.cmake).

add_library(ct_adapter_aeron STATIC
    src/AeronClusterSession.cpp)
add_library(cluster_tools::adapter_aeron ALIAS ct_adapter_aeron)

add_dependencies(ct_adapter_aeron ct_sbe_generate)

target_include_directories(ct_adapter_aeron PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(ct_adapter_aeron
    PUBLIC ct_ports aeron_static Threads::Threads
    PRIVATE ct_sbe_codecs aeron_client_wrapper)

target_compile_options(ct_adapter_aeron PRIVATE -Wall -Wextra)

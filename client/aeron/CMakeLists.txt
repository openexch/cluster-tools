# client/aeron (WITH_AERON) - the ClusterClient template, the Aeron transport, fatal-error
# handling and the core session registrar, with their tests.

# Header-only state machine + registrar: needs the generated cluster and money codecs.
add_library(ct_client INTERFACE)
add_library(cluster_tools::client ALIAS ct_client)
target_link_libraries(ct_client INTERFACE ct_client_core ct_sbe_codecs)

# Aeron-linked part.
add_library(ct_client_aeron STATIC src/FatalErrors.cpp)
add_library(cluster_tools::client_aeron ALIAS ct_client_aeron)
target_include_directories(ct_client_aeron PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ct_client_aeron PUBLIC ct_client aeron_client_wrapper aeron_static Threads::Threads)
target_compile_options(ct_client_aeron PRIVATE -Wall -Wextra)

if(BUILD_TESTING)
    set(ct_client_test_dir "${CMAKE_CURRENT_SOURCE_DIR}/../tests")

    add_executable(ct_client_tests "${ct_client_test_dir}/ClusterClientTest.cpp")
    target_link_libraries(ct_client_tests PRIVATE ct_client)
    target_include_directories(ct_client_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party" "${ct_client_test_dir}")
    target_compile_options(ct_client_tests PRIVATE -Wall -Wextra)
    add_test(NAME client.state_machine COMMAND ct_client_tests)

    add_executable(ct_registrar_tests "${ct_client_test_dir}/CoreSessionRegistrarTest.cpp")
    target_link_libraries(ct_registrar_tests PRIVATE ct_client)
    target_include_directories(ct_registrar_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party" "${ct_client_test_dir}")
    target_compile_options(ct_registrar_tests PRIVATE -Wall -Wextra)
    add_test(NAME client.core_registrar COMMAND ct_registrar_tests)

    add_executable(ct_fatal_tests tests/FatalErrorsTest.cpp)
    target_link_libraries(ct_fatal_tests PRIVATE ct_client_aeron)
    target_include_directories(ct_fatal_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    add_test(NAME client.fatal_errors COMMAND ct_fatal_tests)
endif()

add_subdirectory(../it it)

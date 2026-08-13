# ports — the seams. Header-only; nothing here may include a transport or codec header.
# The tests run in the no-aeron build and compile-prove exactly that.

add_library(ct_ports INTERFACE)
add_library(cluster_tools::ports ALIAS ct_ports)

target_include_directories(ct_ports INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(BUILD_TESTING)
    add_executable(ct_ports_tests tests/PortsTest.cpp)
    target_link_libraries(ct_ports_tests PRIVATE ct_ports)
    target_include_directories(ct_ports_tests PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    target_compile_options(ct_ports_tests PRIVATE -Wall -Wextra)
    add_test(NAME ports.cluster_session COMMAND ct_ports_tests)
endif()

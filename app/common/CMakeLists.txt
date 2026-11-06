# app/common - the process machinery ct-forwarder and ct-bridge share (RC2 S9c W1, G7: moved out of app/forwarder,
# one copy): Supervisor (main's sigtimedwait loop, watchdog, stop path, exit seam, NonBlockingLogWriter), HttpServer,
# LogRing, StatusBoardT, IdleStrategy, DriverTimeout. Pure POSIX: every configuration (the WITH_AERON=OFF job too).
# WITH_AERON: ConductorWatch (the client-conductor heartbeat check, 3.4 FIX-5) in ct_app_common_aeron, and
# EmbeddedDriver.cpp (RC2 S10' 8.2 rule 4: the embedded media driver of ct-bridge AND ct-forwarder), which each final
# target compiles itself (CT_EMBEDDED_DRIVER_SOURCE) and links with aeron_driver_static FIRST: a library here would let
# CMake order aeron_static's client objects ahead of the driver's.

add_library(ct_app_common STATIC HttpServer.cpp Supervisor.cpp)
target_include_directories(ct_app_common PUBLIC "${PROJECT_SOURCE_DIR}/app")
target_link_libraries(ct_app_common PUBLIC Threads::Threads)
target_compile_options(ct_app_common PRIVATE -Wall -Wextra)

if(WITH_AERON)
    add_library(ct_app_common_aeron STATIC ConductorWatch.cpp)
    target_link_libraries(ct_app_common_aeron PUBLIC ct_app_common ct_client_aeron)
    target_compile_options(ct_app_common_aeron PRIVATE -Wall -Wextra)
endif()

set(CT_EMBEDDED_DRIVER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/EmbeddedDriver.cpp" CACHE INTERNAL "app/common's embedded driver")

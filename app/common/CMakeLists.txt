# app/common - the process machinery ct-forwarder and ct-bridge share (RC2 S9c W1, G7: moved out of app/forwarder,
# one copy): Supervisor (main's sigtimedwait loop, watchdog, stop path, exit seam, NonBlockingLogWriter), HttpServer,
# LogRing, StatusBoardT, IdleStrategy, DriverTimeout. Pure POSIX: every configuration (the WITH_AERON=OFF job too).
# WITH_AERON: ConductorWatch (the client-conductor heartbeat check, 3.4 FIX-5) in ct_app_common_aeron.

add_library(ct_app_common STATIC HttpServer.cpp Supervisor.cpp)
target_include_directories(ct_app_common PUBLIC "${PROJECT_SOURCE_DIR}/app")
target_link_libraries(ct_app_common PUBLIC Threads::Threads)
target_compile_options(ct_app_common PRIVATE -Wall -Wextra)

if(WITH_AERON)
    add_library(ct_app_common_aeron STATIC ConductorWatch.cpp)
    target_link_libraries(ct_app_common_aeron PUBLIC ct_app_common ct_client_aeron)
    target_compile_options(ct_app_common_aeron PRIVATE -Wall -Wextra)
endif()

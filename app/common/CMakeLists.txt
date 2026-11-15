# app/common - what ct-forwarder and ct-bridge share that is not process machinery: the table of environment
# variables retired by the rename of the engine to core (RetiredSettings.h). The process machinery itself (Supervisor,
# HttpServer, LogRing, StatusBoardT, IdleStrategy, DriverTimeout, DirProbe, StorageStarvation, ConductorWatch,
# EmbeddedDriver) is the runtime library (runtime/), which other repositories link as cluster_tools::runtime*.
# ct_app_common carries the app/ include root and links ct_runtime, so both processes keep one link name.

add_library(ct_app_common INTERFACE)
target_include_directories(ct_app_common INTERFACE "${PROJECT_SOURCE_DIR}/app")
target_link_libraries(ct_app_common INTERFACE ct_runtime)

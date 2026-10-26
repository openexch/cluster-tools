# poc - the original flat proof of concept, compiled UNCHANGED against the same generated
# codecs and Aeron build as everything else, so it keeps building (it is the reference the
# e2e proof's CreateOrder/OrderStatus probe runs).
add_executable(ct_poc poc.cpp)
target_link_libraries(ct_poc PRIVATE ct_sbe_codecs aeron_client_wrapper aeron_static Threads::Threads)

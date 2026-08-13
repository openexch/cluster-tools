# app/switch-behavior — thin shell composing ports + adapters (and, once the apply
# subcommand lands, domain/SwitchDecision). Only `probe` works today.

add_executable(switch-behavior main.cpp)
target_link_libraries(switch-behavior PRIVATE ct_adapter_aeron)
target_compile_options(switch-behavior PRIVATE -Wall -Wextra)

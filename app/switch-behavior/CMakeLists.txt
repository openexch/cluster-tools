# app/switch-behavior — thin shell composing domain + ports + adapters. Subcommands
# `probe` and `send-config` work today; the SwitchBehavior apply flow (which will use
# domain/SwitchDecision) is still to land.

add_executable(switch-behavior main.cpp)
target_link_libraries(switch-behavior PRIVATE ct_adapter_aeron ct_adapter_json ct_adapter_sbe)
target_compile_options(switch-behavior PRIVATE -Wall -Wextra)

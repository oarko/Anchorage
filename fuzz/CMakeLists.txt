# libFuzzer targets. Requires clang; enable with -DANCHORAGE_FUZZ=ON (see the "fuzz" preset).
add_executable(fstab_fuzzer fstab_fuzzer.cpp)
target_link_libraries(fstab_fuzzer PRIVATE anchorage_core anchorage_hardening)
target_link_options(fstab_fuzzer PRIVATE -fsanitize=fuzzer)

add_executable(policy_fuzzer policy_fuzzer.cpp)
target_link_libraries(policy_fuzzer PRIVATE anchorage_core anchorage_hardening)
target_link_options(policy_fuzzer PRIVATE -fsanitize=fuzzer)

add_executable(options_fuzzer options_fuzzer.cpp)
target_link_libraries(options_fuzzer PRIVATE anchorage_core anchorage_hardening)
target_link_options(options_fuzzer PRIVATE -fsanitize=fuzzer)

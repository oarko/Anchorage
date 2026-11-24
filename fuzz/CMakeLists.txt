# libFuzzer targets. Requires clang; enable with -DANCHORAGE_FUZZ=ON (see the "fuzz" preset).
add_executable(fstab_fuzzer fstab_fuzzer.cpp)
target_link_libraries(fstab_fuzzer PRIVATE anchorage_core anchorage_hardening)
target_link_options(fstab_fuzzer PRIVATE -fsanitize=fuzzer)

# End-to-end test client (built only with -DANCHORAGE_E2E=ON, never installed).
add_executable(anchorage-e2e-client client.cpp)
target_link_libraries(anchorage-e2e-client PRIVATE anchorage_core anchorage_hardening KF6::AuthCore Qt6::DBus)

add_executable(anchorage-e2e-scan scan.cpp)
target_link_libraries(anchorage-e2e-scan PRIVATE anchorage_core anchorage_hardening)

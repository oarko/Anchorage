ecm_add_tests(
    pathstest.cpp
    fstabtest.cpp
    policytest.cpp
    safefstest.cpp
    servicetest.cpp
    LINK_LIBRARIES anchorage_core anchorage_hardening Qt6::Test
)

target_compile_definitions(fstabtest PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

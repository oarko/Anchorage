ecm_add_tests(
    pathstest.cpp
    fstabtest.cpp
    policytest.cpp
    safefstest.cpp
    servicetest.cpp
    inventorytest.cpp
    optionstest.cpp
    mountplantest.cpp
    bindtest.cpp
    LINK_LIBRARIES anchorage_core anchorage_hardening Qt6::Test
)

ecm_add_test(guitest.cpp LINK_LIBRARIES anchorage_ui anchorage_hardening Qt6::Test)

foreach(t fstabtest inventorytest guitest)
    target_compile_definitions(${t} PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
endforeach()

# Real mounts; skips unless run by tests/docker/run-mount-tests.sh (privileged container).
ecm_add_test(mounttest.cpp LINK_LIBRARIES anchorage_core anchorage_hardening Qt6::Test)

set_tests_properties(guitest PROPERTIES ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/guitest.lsan.supp:print_suppressions=0")

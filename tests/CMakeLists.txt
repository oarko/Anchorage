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
    clienttoolstest.cpp
    secretstest.cpp
    LINK_LIBRARIES anchorage_core anchorage_hardening Qt6::Test
)

ecm_add_test(guitest.cpp LINK_LIBRARIES anchorage_ui anchorage_hardening Qt6::Test)

foreach(t fstabtest inventorytest guitest)
    target_compile_definitions(${t} PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
endforeach()

# Real mounts; skips unless run by tests/docker/run-mount-tests.sh (privileged container).
ecm_add_test(mounttest.cpp LINK_LIBRARIES anchorage_core anchorage_hardening Qt6::Test)

set_tests_properties(guitest PROPERTIES ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/guitest.lsan.supp:print_suppressions=0")

# Share listing (SMB through libsmbclient, NFS through showmount). The live part needs
# ANCHORAGE_NAS_ENV; without it, it is skipped.
ecm_add_test(sharestest.cpp LINK_LIBRARIES anchorage_client anchorage_hardening Qt6::Test)

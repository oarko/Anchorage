ecm_add_tests(
    pathstest.cpp
    fstabtest.cpp
    policytest.cpp
    safefstest.cpp
    servicetest.cpp
    inventorytest.cpp
    LINK_LIBRARIES anchorage_core anchorage_hardening Qt6::Test
)

ecm_add_test(guitest.cpp LINK_LIBRARIES anchorage_ui anchorage_hardening Qt6::Test)

foreach(t fstabtest inventorytest guitest)
    target_compile_definitions(${t} PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
endforeach()

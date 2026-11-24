# anchorage_core: logic shared by the GUI, the root helper and the tests.
# No GUI dependencies allowed here, so the helper can link it.
add_library(anchorage_core STATIC
    caller.cpp
    caller.h
    fstabservice.cpp
    fstabservice.h
    mountpolicy.cpp
    mountpolicy.h
    result.h
    safefs.cpp
    safefs.h
    fstab.cpp
    fstab.h
    paths.h
    privilege.cpp
    privilege.h
    requests.cpp
    requests.h
)

target_include_directories(anchorage_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/src
)

target_link_libraries(anchorage_core
    PUBLIC Qt6::Core
    PRIVATE anchorage_hardening
)

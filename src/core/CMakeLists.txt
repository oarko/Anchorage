# anchorage_core: logic shared by the GUI, the root helper and the tests.
# No GUI dependencies allowed here, so the helper can link it.
add_library(anchorage_core STATIC
    access.cpp
    access.h
    linediff.cpp
    linediff.h
    options.cpp
    options.h
    verify.cpp
    verify.h
    caller.cpp
    caller.h
    drives.cpp
    drives.h
    inventory.cpp
    inventory.h
    mounts.cpp
    mounts.h
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

# The option catalogue (SPEC §8, §11), compiled in as string constants. Editing a JSON
# file re-runs CMake, which regenerates catalogue-data.cpp.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/data/filesystems.json
    ${CMAKE_CURRENT_SOURCE_DIR}/data/options.json)
file(READ data/filesystems.json ANCHORAGE_FILESYSTEMS_JSON)
file(READ data/options.json ANCHORAGE_OPTIONS_JSON)
configure_file(catalogue-data.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/catalogue-data.cpp @ONLY)
target_sources(anchorage_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/catalogue-data.cpp)
# GCC and clang accept long literals; the 64 KiB "minimum" warning doesn't apply here.
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/catalogue-data.cpp PROPERTIES COMPILE_OPTIONS -Wno-overlength-strings)

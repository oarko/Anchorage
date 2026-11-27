# anchorage_client: the GUI side of the KAuth helper calls (shared with the e2e client).
add_library(anchorage_client STATIC
    helperclient.cpp
    helperclient.h
    packagekit.cpp
    packagekit.h
    shares.cpp
    shares.h
)
set_target_properties(anchorage_client PROPERTIES AUTOMOC ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SMBCLIENT REQUIRED IMPORTED_TARGET smbclient)
target_include_directories(anchorage_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(anchorage_client
    PUBLIC anchorage_core Qt6::Core
    PRIVATE KF6::AuthCore Qt6::Gui Qt6::DBus PkgConfig::SMBCLIENT anchorage_hardening
)

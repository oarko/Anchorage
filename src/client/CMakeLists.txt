# anchorage_client: the GUI side of the KAuth helper calls (shared with the e2e client).
add_library(anchorage_client STATIC
    helperclient.cpp
    helperclient.h
)
target_include_directories(anchorage_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(anchorage_client
    PUBLIC anchorage_core Qt6::Core
    PRIVATE KF6::AuthCore Qt6::Gui anchorage_hardening
)

# The GUI as a static library, so tests can build the main window with fixture data.
add_library(anchorage_ui STATIC
    anchorage.qrc
    backupspage.cpp
    backupspage.h
    datasource.cpp
    datasource.h
    infopanel.cpp
    infopanel.h
    inventorymodel.cpp
    inventorymodel.h
    mainwindow.cpp
    mainwindow.h
    placespanel.cpp
    placespanel.h
    tiledelegate.cpp
    tiledelegate.h
)
target_include_directories(anchorage_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(anchorage_ui
    PUBLIC
        anchorage_core
        Qt6::Widgets
        KF6::CoreAddons
        KF6::I18n
        KF6::XmlGui
        KF6::WidgetsAddons
        KF6::ItemViews
        KF6::ColorScheme
        KF6::Solid
    PRIVATE
        anchorage_hardening
)

add_executable(${APP_BINARY} main.cpp)
target_link_libraries(${APP_BINARY} PRIVATE anchorage_ui anchorage_hardening)

configure_file(app.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.desktop)

install(TARGETS ${APP_BINARY} ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.desktop DESTINATION ${KDE_INSTALL_APPDIR})

add_executable(${APP_BINARY}
    main.cpp
    mainwindow.cpp
    mainwindow.h
    anchorage.qrc
)

target_link_libraries(${APP_BINARY} PRIVATE
    anchorage_core
    anchorage_hardening
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::XmlGui
)

configure_file(app.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.desktop)

install(TARGETS ${APP_BINARY} ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.desktop DESTINATION ${KDE_INSTALL_APPDIR})

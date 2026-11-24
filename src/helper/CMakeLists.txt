# Privileged helper (runs as root via KAuth/polkit).
add_executable(${APP_HELPER} helper.cpp)
target_link_libraries(${APP_HELPER} PRIVATE
    anchorage_core
    anchorage_hardening
    KF6::AuthCore
)

configure_file(anchorage.actions.in ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.actions)
configure_file(anchorage-groups.rules.in ${CMAKE_CURRENT_BINARY_DIR}/60-${APP_BINARY}-groups.rules @ONLY)

kauth_install_helper_files(${APP_HELPER} ${APP_ID} root)
kauth_install_actions(${APP_ID} ${CMAKE_CURRENT_BINARY_DIR}/${APP_ID}.actions)
install(TARGETS ${APP_HELPER} DESTINATION ${KAUTH_HELPER_INSTALL_DIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/60-${APP_BINARY}-groups.rules DESTINATION ${KDE_INSTALL_DATADIR}/polkit-1/rules.d)

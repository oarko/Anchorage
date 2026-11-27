cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# Naming: the ONLY place the app's name and IDs are defined. Everything else
# (config header, desktop file, polkit actions, install paths) is generated
# from these. Rename before the first release; polkit IDs and config paths are
# a compatibility promise after that.
# ---------------------------------------------------------------------------
set(APP_NAME        "Anchorage")
set(APP_BINARY      "anchorage")
set(APP_HELPER      "anchorage-helper")
set(APP_ID          "io.github.oarko.anchorage")
set(APP_CONFIG_DIR  "/etc/anchorage")
# Group names (the setup script will let the admin choose them).
set(DRIVEADMIN_GROUP "driveadmins" CACHE STRING "Group allowed full fstab management")
set(NETMOUNT_GROUP   "netmount"    CACHE STRING "Group allowed to mount network shares")

project(Anchorage VERSION 0.9.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Version and beta builds (SPEC §12a). The package version is PROJECT_VERSION plus a
# pre-release suffix: "~beta1", "~rc1", or empty for a release. A "~beta"/"~rc" suffix
# always turns ANCHORAGE_BETA on, so a beta can't ship as a release by mistake.
# debian/rules passes ANCHORAGE_PACKAGE_VERSION from debian/changelog; it must match.
# ---------------------------------------------------------------------------
set(ANCHORAGE_VERSION_SUFFIX "~beta1" CACHE STRING "Pre-release suffix of the version (~betaN, ~rcN, or empty)")
set(ANCHORAGE_VERSION "${PROJECT_VERSION}${ANCHORAGE_VERSION_SUFFIX}")
option(ANCHORAGE_BETA "Beta build: launch warning and more logging" OFF)
if(ANCHORAGE_VERSION MATCHES "~(beta|rc)" AND NOT ANCHORAGE_BETA)
    message(NOTICE "Version ${ANCHORAGE_VERSION} is a pre-release: turning ANCHORAGE_BETA on")
    set(ANCHORAGE_BETA ON CACHE BOOL "Beta build: launch warning and more logging" FORCE)
endif()
if(DEFINED ANCHORAGE_PACKAGE_VERSION AND NOT ANCHORAGE_PACKAGE_VERSION STREQUAL ANCHORAGE_VERSION)
    message(FATAL_ERROR "debian/changelog says ${ANCHORAGE_PACKAGE_VERSION}, but CMakeLists.txt says ${ANCHORAGE_VERSION}: "
                        "update project(VERSION) and ANCHORAGE_VERSION_SUFFIX, or the changelog")
endif()
message(STATUS "Anchorage ${ANCHORAGE_VERSION}, beta build: ${ANCHORAGE_BETA}")

set(QT_MIN_VERSION "6.6.0")
set(KF_MIN_VERSION "6.0.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMAddTests)
include(FeatureSummary)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # used by clang-tidy

option(ANCHORAGE_WERROR   "Treat compiler warnings as errors" ON)
option(ANCHORAGE_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(ANCHORAGE_E2E      "Build the end-to-end KAuth test client (tests/vm/e2e.sh)" OFF)
option(ANCHORAGE_FUZZ     "Build libFuzzer targets (requires clang; implies ANCHORAGE_SANITIZE)" OFF)
if(ANCHORAGE_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ANCHORAGE_FUZZ needs clang: use the \"fuzz\" preset")
    endif()
    set(ANCHORAGE_SANITIZE ON CACHE BOOL "" FORCE)
endif()
include(AnchorageHardening)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Concurrent DBus Widgets Test)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS
    CoreAddons
    I18n
    XmlGui
    WidgetsAddons
    Auth
    ItemViews
    ColorScheme
    Solid
)
# Later phases add: KIO, Wallet, DNSSD, libsmbclient.

configure_file(src/config-anchorage.h.in ${CMAKE_BINARY_DIR}/src/config-anchorage.h)

add_subdirectory(src)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(ANCHORAGE_FUZZ)
    add_subdirectory(fuzz)
endif()
if(ANCHORAGE_E2E)
    add_subdirectory(tests/e2e)
endif()

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)

cmake_minimum_required(VERSION 3.21)

set(EBONHOLD_VERSION "0.9.0" CACHE STRING "Ebonhold Updater version")
project(EbonholdUpdater VERSION ${EBONHOLD_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent)
find_package(LibArchive REQUIRED)

qt_add_executable(EbonholdUpdater
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/AuthManager.h
    src/AuthManager.cpp
    src/UpdateManager.h
    src/UpdateManager.cpp
    src/AddonManager.h
    src/AddonManager.cpp
    src/OptionalContentManager.h
    src/OptionalContentManager.cpp
    src/OptionalContentDialog.h
    src/OptionalContentDialog.cpp
)

target_link_libraries(EbonholdUpdater PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    LibArchive::LibArchive
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(EbonholdUpdater PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS EbonholdUpdater
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES packaging/ebonhold-updater.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
)

install(FILES packaging/ebonhold-updater.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
)

install(FILES THIRD_PARTY_LICENSES.md
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/EbonholdUpdater
)

# linuxdeploy-plugin-qt still uses qmake to find Qt plugins/resources.
find_program(EBONHOLD_QMAKE
    NAMES qmake6 qmake
    HINTS /usr/lib/qt6/bin /usr/lib64/qt6/bin
)

configure_file(
    packaging/BuildAppImage.cmake.in
    ${CMAKE_BINARY_DIR}/BuildAppImage.cmake
    @ONLY
)

add_custom_target(appimage
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/BuildAppImage.cmake
    DEPENDS EbonholdUpdater
    USES_TERMINAL
    COMMENT "Creating EbonholdUpdater AppImage"
)

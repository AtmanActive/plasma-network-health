# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

project(plasma-network-health
    VERSION 1.0.2
    DESCRIPTION "Plasma widget showing the health of your network destinations"
    HOMEPAGE_URL "https://github.com/AtmanActive/plasma-network-health"
    LANGUAGES CXX)

include(GNUInstallDirs)

set(PLASMOID_ID "com.github.atmanactive.networkhealth")

option(BUILD_DAEMON "Build the monitoring backend" ON)
option(INSTALL_PLASMOID "Install the Plasma widget package" ON)
option(INSTALL_SYSTEMD_UNIT "Install the optional systemd user unit" ON)
option(STATIC_LIBSTDCXX "Link libstdc++ and libgcc statically, for portable release builds" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(BUILD_DAEMON)
    add_subdirectory(daemon)
endif()

if(INSTALL_PLASMOID)
    set(PLASMOID_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/plasma/plasmoids/${PLASMOID_ID}")
    install(FILES package/metadata.json DESTINATION "${PLASMOID_INSTALL_DIR}")
    install(DIRECTORY package/contents DESTINATION "${PLASMOID_INSTALL_DIR}")
endif()

if(INSTALL_SYSTEMD_UNIT AND BUILD_DAEMON)
    # Where the unit lands depends on the prefix: /usr/lib/systemd/user for a
    # system install, and for a home-directory prefix the caller should point
    # this at ~/.config/systemd/user, which is the only path systemd reads there.
    set(SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/user"
        CACHE PATH "Directory for the systemd user unit")

    configure_file(systemd/plasma-network-healthd.service.in
                   "${CMAKE_CURRENT_BINARY_DIR}/plasma-network-healthd.service" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/plasma-network-healthd.service"
            DESTINATION "${SYSTEMD_USER_UNIT_DIR}")
endif()

message(STATUS "")
message(STATUS "${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "  backend ........ ${BUILD_DAEMON}")
message(STATUS "  widget ......... ${INSTALL_PLASMOID}")
message(STATUS "  systemd unit ... ${INSTALL_SYSTEMD_UNIT}")
message(STATUS "  prefix ......... ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")

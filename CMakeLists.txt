cmake_minimum_required(VERSION 3.16)
project(QtDatabase VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Set up AUTOMOC and some sensible defaults for runtime execution
# When using Qt 6.3, you can replace the code block below with
# qt_standard_project_setup()
set(CMAKE_AUTOMOC ON)
include(GNUInstallDirs)

find_package(Qt6 REQUIRED COMPONENTS HttpServer Network Sql)

qt_add_executable(QtDatabase
    ApiServer.cpp ApiServer.h
    ApiServer_admin.cpp
    ChargeState.cpp ChargeState.h
    DBManager.cpp DBManager.h
    DBManager_order.cpp
    DBManager_seed.cpp
    DBManager_station.cpp
    DBManager_stats.cpp
    DBManager_user.cpp
    charging_pile.h
    main.cpp
    test.h
)
target_link_libraries(QtDatabase PRIVATE
    Qt::Core
    Qt::HttpServer
    Qt::Network
    Qt::Sql
)

install(TARGETS QtDatabase
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Consider using qt_generate_deploy_app_script() for app deployment if
# the project can use Qt 6.3. In that case rerun qmake2cmake with
# --min-qt-version=6.3.

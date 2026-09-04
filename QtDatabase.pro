QT = core
QT += sql network httpserver

CONFIG += c++17 cmdline
CONFIG -= app_bundle

# SQL(schema/indexes/migrations)以 Qt 资源形式编译进程序
RESOURCES += db.qrc

SOURCES += \
        main.cpp \
        ChargeState.cpp \
        DBManager.cpp \
        DBManager_user.cpp \
        DBManager_station.cpp \
        DBManager_order.cpp \
        DBManager_stats.cpp \
        DBManager_seed.cpp \
        ApiServer.cpp \
        ApiServer_admin.cpp

HEADERS += \
        DBManager.h \
        ChargeState.h \
        ApiServer.h \
        charging_pile.h

# 功能自检在 tests/tests.pro(独立目标, 不编进本程序)

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

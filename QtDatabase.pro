QT = core
QT += sql network httpserver

CONFIG += c++17 cmdline
CONFIG -= app_bundle

# 默认数据库目录: 编译期固定为工程根目录。
# 平台与 ChargerDeviceSimulator 两个 exe 都定义成同一个目录,
# 从而共用根目录下的同一个 charge_platform.db(不传路径时)。
DEFINES += DEFAULT_DB_DIR=\\\"$$clean_path($$PWD)\\\"

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
        DBManager_device.cpp \
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

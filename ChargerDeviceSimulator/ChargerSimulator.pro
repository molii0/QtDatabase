# ============================================================
# ChargerSimulator —— 充电桩设备仿真(Device Side)
# v2: 设备层只与数据库层对接 —— 直接编译平台仓库的 DBManager(数据库层),
#     遥测/心跳/设备状态全部入库; 接口/界面等其他层由数据库层对接,
#     设备不与它们直连。
# ============================================================
QT += core
QT += sql          # 数据库层(SQLite, WAL 多进程并发)
QT -= gui

CONFIG += console c++17
CONFIG -= app_bundle

TEMPLATE = app
TARGET   = ChargerSimulator

# 引用上层 QtDatabase 仓库的数据库层(头文件 + SQL 资源)
INCLUDEPATH += ..
RESOURCES   += ../db.qrc

# 默认数据库目录: 编译期固定为平台仓库根目录(ChargerSimulator.pro 的上层),
# 与平台 exe 共用同一个 charge_platform.db —— 模拟层直接把数据写进平台的库。
DEFINES += DEFAULT_DB_DIR=\\\"$$absolute_path(.., $$PWD)\\\"

SOURCES += \
    src/main.cpp \
    src/charger.cpp \
    src/simulator.cpp \
    src/devicedb.cpp \
    ../ChargeState.cpp \
    ../DBManager.cpp \
    ../DBManager_user.cpp \
    ../DBManager_station.cpp \
    ../DBManager_order.cpp \
    ../DBManager_stats.cpp \
    ../DBManager_seed.cpp \
    ../DBManager_price.cpp \
    ../DBManager_prediction.cpp \
    ../DBManager_device.cpp

HEADERS += \
    src/charger.h \
    src/simulator.h \
    src/devicedb.h \
    ../DBManager.h \
    ../ChargeState.h

# 可选: 固定输出到本目录 bin/ (Qt Creator shadow build 时也会拷贝到这里)
# DESTDIR = $$PWD/bin

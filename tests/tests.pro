# 数据库模块自检程序(独立目标, 不编进产品入口)
QT = core
QT += sql

CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = tst_dbmanager
INCLUDEPATH += ..

# 和主程序共用同一份 SQL 资源(schema / indexes / migrations)
RESOURCES += ../db.qrc

SOURCES += \
        tst_dbmanager.cpp \
        ../ChargeState.cpp \
        ../DBManager.cpp \
        ../DBManager_user.cpp \
        ../DBManager_station.cpp \
        ../DBManager_order.cpp \
        ../DBManager_stats.cpp \
        ../DBManager_seed.cpp \
        ../DBManager_demogen.cpp \
        ../DBManager_price.cpp \
        ../DBManager_device.cpp

HEADERS += \
        ../DBManager.h \
        ../ChargeState.h

# 运行: tst_dbmanager [数据库文件](不传则用系统临时目录测试库)

QT = core
QT += sql network httpserver

CONFIG += c++17 cmdline
CONFIG -= app_bundle

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
        test.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

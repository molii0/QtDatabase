// ============================================================================
// 程序入口
// 两种运行方式:
//   1. 普通演示:  QtDatabase.exe [数据库文件]         -> 跑 test.h 里的功能测试
//   2. REST 服务器: QtDatabase.exe [数据库文件] --server [端口]
//                  -> 启动 QHttpServer 的 RESTful JSON 接口(默认 8080)
// ============================================================================

#include <QCoreApplication>
#include <QDebug>
#include <QDir>

#include "ApiServer.h"
#include "DBManager.h"
#include "test.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 解析命令行: 第一个非 -- 参数是数据库文件; --server 表示启动服务器; 后面的数字是端口
    QString dbPath;
    bool serverMode = false;
    quint16 port = 8080;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--server"))
            serverMode = true;
        else if (dbPath.isEmpty())
            dbPath = arg;
        else
            port = static_cast<quint16>(arg.toInt());
    }
    if (dbPath.isEmpty())
        dbPath = QDir(QCoreApplication::applicationDirPath())
                    .filePath(QStringLiteral("charge_platform.db"));

    DBManager &db = DBManager::instance();
    if (!db.init(dbPath)) {
        qCritical().noquote() << "数据库初始化失败, 程序退出";
        return 1;
    }
    qInfo().noquote() << QStringLiteral("数据库文件: %1").arg(db.dbPath());

    // ---------- REST 服务器模式: 一直运行, 等待前端 HTTP 请求 ----------
    if (serverMode) {
        ApiServer api;
        if (!api.start(port))
            return 1;
        qInfo().noquote() << QStringLiteral("REST 接口服务器已启动: http://127.0.0.1:%1 (Ctrl+C 退出)").arg(port);
        return app.exec();
    }

    // ---------- 演示模式: 跑一遍数据库功能测试 ----------
    qInfo() << "\n--- 各表数据量 ---";
    printTableCount(db, QStringLiteral("\"user\""));
    printTableCount(db, QStringLiteral("admin"));
    printTableCount(db, QStringLiteral("station"));
    printTableCount(db, QStringLiteral("charger"));
    printTableCount(db, QStringLiteral("charging_order"));

    demoUser(db);
    demoAdmin(db);
    demoStation(db);
    demoStationManage(db);
    demoOrderFlow(db);
    demoStats(db);

    qInfo() << "\n演示结束, 数据库模块工作正常。";
    qInfo().noquote() << QStringLiteral("提示: 加参数 --server [端口] 可启动 REST 接口服务器(默认 8080)。");
    return 0;
}

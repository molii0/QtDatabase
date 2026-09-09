// ============================================================================
// 程序入口(产品入口, 不含功能测试)
// 用法:
//   QtDatabase.exe [数据库文件] --server [端口]   启动 REST 接口服务器(默认 8080)
//   QtDatabase.exe [数据库文件]                    只做初始化并提示(数据库层自检请运行 tests/tst_dbmanager)
// 不传数据库文件时, 使用工程根目录的默认库 charge_platform.db
// (由 .pro 的 DEFAULT_DB_DIR 固定, 与 ChargerSimulator 共用同一个库)。
// ============================================================================

#include <QCoreApplication>
#include <QDebug>

#include "ApiServer.h"
#include "DBManager.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString dbPath;
    bool serverMode = true;
    quint16 port = 8080;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--server"))
            serverMode = true;
        else if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
            qInfo().noquote() << QStringLiteral(
                "用法: QtDatabase [数据库文件] [--server [端口]]\n"
                "  --server [端口]  启动 REST 接口服务器(默认 8080)\n"
                "  不加参数          初始化数据库后退出(自检请运行 tests/tst_dbmanager)");
            return 0;
        }
        else if (dbPath.isEmpty())
            dbPath = arg;
        else
            port = static_cast<quint16>(arg.toInt());
    }

    DBManager &db = DBManager::instance();
    if (!db.init(dbPath)) {          // 空路径 -> DBManager 默认库(工程根目录)
        qCritical().noquote() << "数据库初始化失败, 程序退出";
        return 1;
    }
    qInfo().noquote() << QStringLiteral("数据库就绪: %1").arg(db.dbPath());

    if (serverMode) {
        ApiServer api;
        if (!api.start(port))
            return 1;
        qInfo().noquote() << QStringLiteral("REST 接口服务器已启动: http://127.0.0.1:%1 (Ctrl+C 退出)").arg(port);
        return app.exec();
    }

    qInfo().noquote() << QStringLiteral("初始化完成。启动服务器加 --server; 数据库功能自检请运行 tests/tst_dbmanager。");
    return 0;
}

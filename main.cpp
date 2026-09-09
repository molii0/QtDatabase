// ============================================================================
// 程序入口(产品入口, 不含功能测试)
// 用法:
//   QtDatabase.exe [数据库文件] --server [端口]      启动 REST 接口服务器(默认 8080)
//   QtDatabase.exe [数据库文件] --gen-history <N> [--density X]   演示历史数据生成工具
//   QtDatabase.exe [数据库文件]                       只做初始化并提示
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

    QString dbPath;                 // 第一个位置参数: 数据库文件
    quint16 port = 8080;
    int genDays = 0;                // >0 表示进入"演示历史数据生成"模式
    double genDensity = 1.0;

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
            qInfo().noquote() << QStringLiteral(
                "用法: QtDatabase [数据库文件] [--server [端口]]\n"
                "  --server [端口]      启动 REST 接口服务器(默认 8080)\n"
                "  --gen-history <N>    演示数据工具: 把历史(订单/充值/运维日志/负荷预测)\n"
                "                       向前补足到最近 N 天(幂等, 可重复运行)\n"
                "  --density <倍率>      与 --gen-history 搭配: 生成密度 0.1~20(默认 1.0)\n"
                "  不加参数              初始化数据库后退出(自检请运行 tests/tst_dbmanager)");
            return 0;
        } else if (arg == QStringLiteral("--server")) {
            // 后面跟的数字视为端口(可选)
            if (i + 1 < args.size()) {
                bool ok = false;
                const quint16 p = static_cast<quint16>(args.at(i + 1).toUShort(&ok));
                if (ok && p > 0) {
                    port = p;
                    ++i;
                }
            }
        } else if (arg == QStringLiteral("--gen-history")) {
            bool ok = false;
            if (i + 1 < args.size())
                genDays = args.at(++i).toInt(&ok);
            if (!ok || genDays < 1 || genDays > 730) {
                qCritical().noquote() << "--gen-history 需要一个 1~730 的天数";
                return 2;
            }
        } else if (arg == QStringLiteral("--density")) {
            bool ok = false;
            if (i + 1 < args.size())
                genDensity = args.at(++i).toDouble(&ok);
            if (!ok || genDensity < 0.1 || genDensity > 20.0) {
                qCritical().noquote() << "--density 需要 0.1~20 的倍率";
                return 2;
            }
        } else if (arg.startsWith(QLatin1Char('-'))) {
            qInfo().noquote() << "无法识别的参数:" << arg << "(--help 查看用法)";
            return 2;
        } else if (dbPath.isEmpty()) {
            dbPath = arg;           // 第一个位置参数 = 数据库文件
        } else if (genDays == 0) {
            // 兼容旧写法: --server 模式下第二个位置参数曾当作端口
            bool ok = false;
            const quint16 p = static_cast<quint16>(arg.toUShort(&ok));
            if (ok && p > 0)
                port = p;
        }
    }

    DBManager &db = DBManager::instance();
    if (!db.init(dbPath)) {          // 空路径 -> DBManager 默认库(工程根目录)
        qCritical().noquote() << "数据库初始化失败, 程序退出";
        return 1;
    }
    qInfo().noquote() << QStringLiteral("数据库就绪: %1").arg(db.dbPath());

    // ---- 演示历史数据生成工具(离线模式) ----
    if (genDays > 0) {
        DBManager::DemoGenResult r;
        QString ge;
        if (!db.generateDemoHistory(genDays, genDensity, &r, &ge)) {
            qCritical().noquote() << "历史数据生成失败:" << ge;
            return 1;
        }
        if (r.daysGenerated == 0) {
            qInfo().noquote() << QStringLiteral("历史已覆盖最近 %1 天, 无需生成")
                                     .arg(r.daysRequested);
        } else {
            qInfo().noquote() << QStringLiteral(
                "历史数据补生成完成: 新增 %1 天(订单 +%2, 充值流水 +%3,"
                " 运维日志 +%4, 负荷预测 +%5)")
                .arg(r.daysGenerated)
                .arg(r.ordersAdded).arg(r.rechargesAdded)
                .arg(r.opsLogsAdded).arg(r.predictionsAdded);
        }
        return 0;
    }

    // ---- REST 接口服务器(默认模式) ----
    ApiServer api;
    if (!api.start(port))
        return 1;
    qInfo().noquote() << QStringLiteral("REST 接口服务器已启动: http://127.0.0.1:%1 (Ctrl+C 退出)").arg(port);
    return app.exec();
}

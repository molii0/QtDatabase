// ============================================================================
// 程序入口(产品入口, 不含功能测试)
// 用法:
//   QtDatabase.exe [--db 数据库文件] [--server [端口]] [--port 端口]
//   QtDatabase.exe [--db 数据库文件] --gen-history <N> [--density X]   演示历史数据工具
//   QtDatabase.exe --print-db                                          只打印将使用的库路径
// 不传库文件时的默认库(兼容旧用法): 当前目录已有的 charge_platform.db > exe 目录已有的 >
// 工程根目录 charge_platform.db(编译期 DEFAULT_DB_DIR, 不存在会自动建库)。
// 参数解析保持宽松: 不认识的参数只警告, 不会让服务启动失败。
// ============================================================================

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QTextStream>

#include "ApiServer.h"
#include "DBManager.h"

static void printUsage()
{
    QTextStream out(stdout);
    out << "用法: QtDatabase [--db 库文件] [--server [端口]] [--port 端口]\n"
           "  --db <文件>           指定数据库文件(最稳妥, 多进程共用时建议显式指定)\n"
           "  --server [端口]       启动 REST 接口服务器(默认 8080)\n"
           "  --port <端口>         指定服务器端口(等价 --server <端口>)\n"
           "  --print-db            只打印将使用的数据库文件路径后退出(排查用)\n"
           "  --gen-history <N>     演示数据工具: 历史(订单/充值/运维日志/负荷预测)\n"
           "                        向前补足到最近 N 天(幂等, 可重复运行)\n"
           "  --density <倍率>      与 --gen-history 搭配: 生成密度 0.1~20(默认 1.0)\n"
           "  --help                显示本帮助\n"
           "不传库文件时默认: 当前目录已有库 > exe 目录已有库 > 工程根目录 charge_platform.db\n";
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString dbPath;                 // 数据库文件(不传 = 用默认规则)
    quint16 port = 8080;
    int genDays = 0;                // >0 表示进入"演示历史数据生成"模式
    double genDensity = 1.0;
    bool printDbOnly = false;

    auto warn = [](const QString &msg) { qWarning().noquote() << "[WARN]" << msg; };

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        // 取下一个参数值
        auto takeValue = [&](QString *out) -> bool {
            if (i + 1 >= args.size())
                return false;
            *out = args.at(++i);
            return true;
        };

        if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
            printUsage();
            return 0;
        } else if (arg == QStringLiteral("--db")) {
            QString v;
            if (!takeValue(&v)) {
                warn(QStringLiteral("--db 缺少参数值"));
                printUsage();
                return 2;
            }
            dbPath = v;
        } else if (arg == QStringLiteral("--server")) {
            // 后面跟的数字视为端口(可选, 兼容旧写法)
            if (i + 1 < args.size()) {
                bool ok = false;
                const quint16 p = static_cast<quint16>(args.at(i + 1).toUShort(&ok));
                if (ok && p > 0) {
                    port = p;
                    ++i;
                }
            }
        } else if (arg == QStringLiteral("--port") || arg == QStringLiteral("-p")) {
            QString v;
            bool ok = false;
            if (takeValue(&v)) {
                const quint16 p = static_cast<quint16>(v.toUShort(&ok));
                if (ok && p > 0)
                    port = p;
            }
            if (!ok) {
                warn(QStringLiteral("--port 需要 1~65535 的端口号"));
                printUsage();
                return 2;
            }
        } else if (arg == QStringLiteral("--print-db")) {
            printDbOnly = true;
        } else if (arg == QStringLiteral("--gen-history")) {
            QString v;
            bool ok = false;
            if (takeValue(&v))
                genDays = v.toInt(&ok);
            if (!ok || genDays < 1 || genDays > 730) {
                warn(QStringLiteral("--gen-history 需要一个 1~730 的天数"));
                return 2;
            }
        } else if (arg == QStringLiteral("--density")) {
            QString v;
            bool ok = false;
            if (takeValue(&v))
                genDensity = v.toDouble(&ok);
            if (!ok || genDensity < 0.1 || genDensity > 20.0) {
                warn(QStringLiteral("--density 需要 0.1~20 的倍率"));
                return 2;
            }
        } else if (arg.startsWith(QLatin1Char('-'))) {
            // 宽松: 不认识的参数只警告, 不让服务起不来(兼容组员旧启动脚本)
            warn(QStringLiteral("忽略无法识别的参数: %1 (--help 查看用法)").arg(arg));
        } else if (dbPath.isEmpty()) {
            dbPath = arg;           // 第一个位置参数 = 数据库文件(旧写法)
        } else {
            bool ok = false;
            const quint16 p = static_cast<quint16>(arg.toUShort(&ok));
            if (ok && p > 0)
                port = p;           // 旧写法: 第二个位置参数 = 端口
            else
                warn(QStringLiteral("忽略多余的参数: %1").arg(arg));
        }
    }

    // --print-db: 只解析路径, 不建库/不打开(排查"到底连的哪个库")
    if (printDbOnly) {
        const QString path = dbPath.isEmpty() ? DBManager::resolveDefaultDbPath()
                                              : QDir::cleanPath(dbPath);
        QTextStream out(stdout);
        out << "[DB] 将使用的数据库文件: " << path << Qt::endl;
        return 0;
    }

    DBManager &db = DBManager::instance();
    if (!db.init(dbPath)) {
        qCritical().noquote()
            << QStringLiteral("[DB] 数据库初始化失败, 程序退出(可用 --db <文件> 显式指定,"
                              " 或用 --print-db 查看将使用的路径)");
        return 1;
    }
    qInfo().noquote() << QStringLiteral("================ [DB] 数据库就绪: %1 ================")
                             .arg(db.dbPath());

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
    qInfo().noquote() << QStringLiteral(
        "================ REST 接口服务器已启动: http://127.0.0.1:%1  [DB] %2 ================")
        .arg(port).arg(db.dbPath());
    qInfo().noquote() << QStringLiteral("(Ctrl+C 退出; 客户端连不上时先确认本行已出现)");
    return app.exec();
}

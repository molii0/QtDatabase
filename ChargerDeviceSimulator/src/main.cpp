// ============================================================
// ChargerSimulator 入口 —— 充电桩设备仿真(Device Side)
// ------------------------------------------------------------
// 用法:
//   ChargerSimulator [--db FILE] [--station ID] [--devices N]
//                    [--duration SEC] [--auto] [--help]
//     --db FILE      平台数据库文件; 不传则用 DBManager 的默认库 —— 工程根目录
//                    charge_platform.db(与平台 QtDatabase 共用同一个库, WAL 并发)
//     --station ID   只模拟该电站下的电桩(默认不限电站)
//     --devices N    最多绑定的电桩数(默认 3, 上限 30)
//     --duration SEC 运行 SEC 秒后自动退出(自检/演示用)
//     --auto         随机场景自动演示(插枪充电/随机故障/随机掉线)
//
// 设备层只与数据库层对接: 每秒遥测、每 5 秒心跳、状态变化与故障日志
// 全部写入数据库(charger_telemetry / charger_heartbeat / charger.status /
// ops_log); 平台下发的命令经 device_command 表轮询执行。设备不与
// 接口层/界面层等其他层直连, 其他层的数据一律经由数据库层读取。
// ============================================================

#include "simulator.h"

#include <QCoreApplication>
#include <QDir>
#include <QStringList>
#include <QTextStream>

static void printUsage()
{
    QTextStream out(stdout);
    out << "ChargerSimulator v2 - 充电桩设备仿真(Device Side, 只与数据库层对接)\n"
           "用法:\n"
           "  ChargerSimulator [--db FILE] [--station ID] [--devices N]\n"
           "                   [--duration SEC] [--auto] [--print-db] [--help]\n"
           "    --db FILE      平台数据库文件; 不传则按默认规则挑: 当前目录已有库 >\n"
           "                   exe 目录已有库 > 工程根目录 charge_platform.db(会自动建库)\n"
           "    --print-db     只打印将使用的数据库文件路径后退出(排查用)\n"
           "    --station ID   只模拟该电站下的电桩(默认不限电站)\n"
           "    --devices N    最多绑定电桩数, 默认 3 (上限 30)\n"
           "    --duration SEC 运行 SEC 秒后自动退出; 默认不限(可输入 exit 退出)\n"
           "    --auto         随机场景自动演示(插枪充电/随机故障/随机掉线)\n"
           "\n"
           "运行中命令(id 为数据库电桩编号 charger_id):\n"
           "  list                   查看所有设备状态\n"
           "  db                     查看数据库对接信息与已发送帧数\n"
           "  plug <id>              插枪        (idle -> reserved)\n"
           "  start <id>             开始充电    (reserved -> charging)\n"
           "  stop <id>              手动结束    (charging -> finished)\n"
           "  unplug <id>            拔枪        (finished -> idle)\n"
           "  fault <id> [code]      模拟故障    (任意 -> fault)\n"
           "  recover <id>           故障恢复    (fault -> idle)\n"
           "  offline <id>           模拟掉线    (任意 -> offline, 停止遥测/心跳)\n"
           "  online <id>            恢复在线    (offline -> idle)\n"
           "  restart <id>           重启设备    (任意 -> offline ->(2秒)-> idle)\n"
           "  exit                   退出程序\n";
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    SimulatorOptions opt;
    QString dbArg;
    bool printDbOnly = false;

    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        auto readInt = [&](qint64 &outVal) {
            bool ok = false;
            if (i + 1 < args.size())
                outVal = args.at(++i).toLongLong(&ok);
            return ok;
        };
        if (a == QStringLiteral("--help") || a == QStringLiteral("-h")) {
            printUsage();
            return 0;
        } else if (a == QStringLiteral("--db")) {
            if (i + 1 >= args.size()) { printUsage(); return 2; }
            dbArg = args.at(++i);
        } else if (a == QStringLiteral("--print-db")) {
            printDbOnly = true;
        } else if (a == QStringLiteral("--station")) {
            qint64 v = 0;
            if (!readInt(v) || v <= 0) { printUsage(); return 2; }
            opt.stationId = v;
        } else if (a == QStringLiteral("--devices") || a == QStringLiteral("-n")) {
            qint64 v = 0;
            if (!readInt(v) || v <= 0 || v > 30) { printUsage(); return 2; }
            opt.deviceCount = static_cast<int>(v);
        } else if (a == QStringLiteral("--duration") || a == QStringLiteral("-d")) {
            qint64 v = 0;
            if (!readInt(v) || v <= 0) { printUsage(); return 2; }
            opt.durationSec = static_cast<int>(v);
        } else if (a == QStringLiteral("--auto")) {
            opt.autoMode = true;
        } else {
            // 宽松: 不认识的参数只警告, 不直接退出(兼容旧启动脚本)
            QTextStream(stderr) << "[WARN] 忽略无法识别的参数: " << a
                                << " (--help 查看用法)\n";
        }
    }

    // --print-db: 只打印将使用的数据库文件(不建库/不打开), 排查"连的哪个库"
    if (printDbOnly) {
        const QString path = dbArg.isEmpty() ? DBManager::resolveDefaultDbPath()
                                             : QDir::cleanPath(dbArg);
        QTextStream(stdout) << "[DB] 将使用的数据库文件: " << path << Qt::endl;
        return 0;
    }

    // --db 未指定时留空: DBManager 按默认规则挑库(当前目录/exe 目录已存在的库优先,
    // 都没有才用工程根目录), 保证与平台共用同一个库。
    opt.dbPath = dbArg;

    Simulator sim(opt);
    QString err;
    if (!sim.init(&err)) {
        QTextStream out(stderr);
        out << "[ERROR] " << err << "\n";
        return 1;
    }
    sim.start();
    return app.exec();
}

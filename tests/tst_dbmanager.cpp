// ============================================================================
// 数据库模块自检程序(独立于主程序, 不编进产品入口)
//   运行: tst_dbmanager [数据库文件]   (不传则用系统临时目录的测试库)
// 覆盖: 初始化/种子、用户/管理员、电站桩管理、订单状态机与 BR-02、结算、
//       统计、运维日志、schema 版本与唯一索引。
// 用法: 在 Qt Creator 里打开 tests/tests.pro 构建运行; 或命令行直接运行。
// ============================================================================

#include "DBManager.h"

#include <QCoreApplication>
#include <QDate>
#include <QDebug>
#include <QDir>
#include <QSqlQuery>
#include <QThread>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name)                                                        \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_pass;                                                            \
            qInfo().noquote() << QStringLiteral("[OK ]") << name;                \
        } else {                                                                 \
            ++g_fail;                                                            \
            qWarning().noquote() << QStringLiteral("[FAIL]") << name;            \
        }                                                                        \
    } while (0)

static qint64 rowCount(DBManager &db, const QString &table)
{
    QSqlQuery q(db.db());
    q.exec(QStringLiteral("SELECT COUNT(*) FROM %1;").arg(table));
    return q.next() ? q.value(0).toLongLong() : -1;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // 测试库路径: 参数 > 系统临时目录; 先删掉旧文件保证每次都是全新库
    const QString dbPath = argc > 1
        ? QString::fromLocal8Bit(argv[1])
        : QDir::temp().filePath(QStringLiteral("ncs_tst_dbmanager.db"));
    QFile::remove(dbPath);
    QFile::remove(dbPath + QStringLiteral("-wal"));
    QFile::remove(dbPath + QStringLiteral("-shm"));

    DBManager &db = DBManager::instance();
    CHECK(db.init(dbPath), QStringLiteral("初始化全新库"));

    // ---------------- 基础结构 ----------------
    int version = 0;
    QSqlQuery q(db.db());
    q.exec(QStringLiteral("SELECT MAX(version) FROM schema_version;"));
    if (q.next()) version = q.value(0).toInt();
    CHECK(version >= 7, QStringLiteral("schema_version = %1").arg(version));

    q.exec(QStringLiteral(
        "SELECT name FROM sqlite_master WHERE type='index' AND name LIKE 'uq_%' ORDER BY name;"));
    QStringList uq;
    while (q.next()) uq << q.value(0).toString();
    CHECK(uq.contains(QStringLiteral("uq_order_user_active"))
              && uq.contains(QStringLiteral("uq_order_charger_active")),
          QStringLiteral("BR-02/03 唯一索引存在: %1").arg(uq.join(QLatin1Char(','))));

    CHECK(rowCount(db, QStringLiteral("\"user\"")) == 50, QStringLiteral("种子用户 50 个"));
    CHECK(rowCount(db, QStringLiteral("station")) == 50, QStringLiteral("种子电站 50 座"));
    CHECK(rowCount(db, QStringLiteral("charger")) == 300, QStringLiteral("种子电桩 300 台"));
    CHECK(rowCount(db, QStringLiteral("ops_log")) >= 0, QStringLiteral("ops_log 表存在"));
    CHECK(rowCount(db, QStringLiteral("charger_telemetry")) >= 0
              && rowCount(db, QStringLiteral("charger_heartbeat")) >= 0
              && rowCount(db, QStringLiteral("device_command")) >= 0,
          QStringLiteral("设备接入表存在(v7)"));

    // ---------------- 用户 / 管理员 ----------------
    DBManager::User u;
    CHECK(db.getUserByPhone(QStringLiteral("12345678910"), &u)
              && u.nickname == QStringLiteral("测试用户"),
          QStringLiteral("按手机号查用户"));
    CHECK(db.insertUser(QStringLiteral("13800001234"), QStringLiteral("新用户")),
          QStringLiteral("注册新用户"));
    CHECK(!db.insertUser(QStringLiteral("13800001234"), QStringLiteral("重复")),
          QStringLiteral("重复手机号注册被拒"));
    qint64 newId = 0;
    db.getUserByPhone(QStringLiteral("13800001234"), &u);
    newId = u.userId;
    CHECK(db.updateBalance(newId, 30.0), QStringLiteral("充值 30 元"));
    db.getUserById(newId, &u);
    CHECK(qAbs(u.balance - 30.0) < 0.001, QStringLiteral("充值后余额 30.00"));

    CHECK(db.checkAdminLogin(QStringLiteral("admin"), QStringLiteral("123456")),
          QStringLiteral("管理员登录成功"));
    CHECK(!db.checkAdminLogin(QStringLiteral("admin"), QStringLiteral("bad")),
          QStringLiteral("错误密码被拒"));

    // 冻结/解冻
    QString e;
    CHECK(db.setUserStatus(newId, true, &e), QStringLiteral("冻结用户"));
    db.getUserById(newId, &u);
    CHECK(u.status == 0, QStringLiteral("用户已冻结"));
    CHECK(db.setUserStatus(newId, false, &e), QStringLiteral("解冻用户"));

    // ---------------- 电站 / 电桩管理 ----------------
    QVector<DBManager::Station> stations = db.listStations();
    CHECK(stations.size() == 50, QStringLiteral("电站列表 50 座"));
    qint64 stationId = 0;
    CHECK(db.addStation(QStringLiteral("测试电站"), QStringLiteral("CS"),
                        QStringLiteral("测试地址"), 123.0, 41.0, 1.1, &stationId, &e),
          QStringLiteral("新增电站"));
    int created = 0;
    CHECK(db.batchAddChargers(stationId, 3, 0, 7.0, &created, &e) && created == 3,
          QStringLiteral("批量建桩 3 台"));
    QVector<DBManager::Charger> cs = db.listChargers(stationId);
    CHECK(cs.size() == 3 && cs.first().code == QStringLiteral("CS-01"),
          QStringLiteral("桩编号自动顺延 CS-01~CS-03"));
    CHECK(!db.deleteStation(stationId, &e), QStringLiteral("有桩删站被拒(BR-10)"));
    for (const DBManager::Charger &c : cs)
        db.deleteCharger(c.chargerId, &e);          // 无订单可删
    CHECK(db.deleteStation(stationId, &e), QStringLiteral("清桩后删站成功"));

    // ---------------- 订单流程(状态机 + BR-02/03) ----------------
    DBManager::User user4;
    db.getUserByPhone(QStringLiteral("13900000003"), &user4);   // 无种子活动订单的用户
    DBManager::Charger idle;
    const QVector<DBManager::Charger> all = db.listChargers();
    for (const DBManager::Charger &c : all) {
        if (c.status == ChargerIdle) { idle = c; break; }
    }
    CHECK(idle.chargerId != 0, QStringLiteral("存在空闲桩"));

    qint64 orderId = 0;
    CHECK(db.orderConnect(user4.userId, idle.chargerId, &orderId, &e),
          QStringLiteral("下单成功(待支付/已连接)"));

    // BR-02: 未结算时再下单被拒
    DBManager::Charger idle2;
    for (const DBManager::Charger &c : all) {
        if (c.status == ChargerIdle && c.chargerId != idle.chargerId) { idle2 = c; break; }
    }
    if (idle2.chargerId) {
        CHECK(!db.orderConnect(user4.userId, idle2.chargerId, nullptr, &e),
              QStringLiteral("未结算重复下单被拒(BR-02): %1").arg(e));
    }

    // 非法状态转换: 待支付直接结算
    CHECK(!db.orderFinish(orderId, &e), QStringLiteral("待支付不能直接结算(状态机)"));

    // 开始充电 -> 结算
    CHECK(db.orderStart(orderId, &e), QStringLiteral("开始充电"));
    QThread::msleep(1100);   // 等 1.1 秒, 保证"充电时长 > 0"
    CHECK(db.orderFinish(orderId, &e), QStringLiteral("结算成功: %1").arg(e));

    DBManager::Order done;
    db.getOrderById(orderId, &done);
    CHECK(done.status == OrderFinished && !done.endTime.isEmpty(),
          QStringLiteral("订单已完成并写入结束时间"));
    DBManager::Charger after;
    db.getCharger(idle.chargerId, &after);
    CHECK(after.status == ChargerIdle, QStringLiteral("电桩已释放为空闲"));

    // ---------------- BR-04/BR-06 闭环: 起充金额、欠费禁充、充值先还款 ----------------
    {
        DBManager::User poor;
        CHECK(db.insertUser(QStringLiteral("13900000009"), QStringLiteral("欠费用户")),
              QStringLiteral("注册新用户"));
        db.getUserByPhone(QStringLiteral("13900000009"), &poor);

        // ① BR-04: 余额不足起充金额(5元)不能下单
        QString errDebt;
        DBManager::Charger anyIdle;
        const QVector<DBManager::Charger> chargers0 = db.listChargers();
        for (const DBManager::Charger &c : chargers0) {
            if (c.status == ChargerIdle) { anyIdle = c; break; }
        }
        CHECK(anyIdle.chargerId != 0 && !db.orderConnect(poor.userId, anyIdle.chargerId, nullptr, &errDebt)
                  && errDebt.contains(QStringLiteral("起充")),
              QStringLiteral("余额<起充金额被拒(BR-04): %1").arg(errDebt));

        // ② 给 5 元起充下单并开始, 再模拟"充电中把余额花光"
        CHECK(db.updateBalance(poor.userId, 5.0), QStringLiteral("充值到 5 元"));
        DBManager::Charger fast;
        const QVector<DBManager::Charger> chargers = db.listChargers();
        for (const DBManager::Charger &c : chargers) {
            if (c.status == ChargerIdle && c.power >= 100.0) { fast = c; break; }
        }
        qint64 poorOrder = 0;
        CHECK(db.orderConnect(poor.userId, fast.chargerId, &poorOrder, &e)
                  && db.orderStart(poorOrder, &e),
              QStringLiteral("余额 5 元下单并开始充电"));
        CHECK(db.updateBalance(poor.userId, -5.0), QStringLiteral("(模拟)充电中余额被花光"));
        QThread::msleep(1200);

        // ③ BR-06: 余额不足也能结算 -> 实扣0, 欠费=应付, 并累计到 user.debt
        CHECK(db.orderFinish(poorOrder, &e),
              QStringLiteral("余额不足结算不应被拒(BR-06): %1").arg(e));
        DBManager::Order poorDone;
        db.getOrderById(poorOrder, &poorDone);
        CHECK(poorDone.status == OrderFinished && poorDone.amount > 0
                  && poorDone.paid == 0.0
                  && qAbs(poorDone.debt - poorDone.amount) < 0.001,
              QStringLiteral("订单完成: 应付 %1, 实扣 0, 欠费 %2")
                  .arg(poorDone.amount, 0, 'f', 2).arg(poorDone.debt, 0, 'f', 2));
        db.getUserById(poor.userId, &poor);
        CHECK(qAbs(poor.balance) < 0.001 && qAbs(poor.debt - poorDone.debt) < 0.001,
              QStringLiteral("余额=0, 未结清欠费= %1").arg(poor.debt, 0, 'f', 2));

        // ④ 欠费禁充: 有未结清欠费再下单被拒
        DBManager::Charger fast2;
        for (const DBManager::Charger &c : chargers) {
            if (c.status == ChargerIdle && c.power >= 100.0 && c.chargerId != fast.chargerId) {
                fast2 = c; break;
            }
        }
        errDebt.clear();
        if (fast2.chargerId) {
            CHECK(!db.orderConnect(poor.userId, fast2.chargerId, nullptr, &errDebt)
                      && errDebt.contains(QStringLiteral("欠费")),
                  QStringLiteral("有欠费再下单被拒: %1").arg(errDebt));
        }

        // ⑤ 充值先还款: 还清欠费后余额 >= 起充, 可再次下单
        double repay = 0.0, remain = -1.0;
        const double topUp = poor.debt + 5.0;
        CHECK(db.recharge(poor.userId, topUp, &repay, &remain, &e),
              QStringLiteral("充值 %1 元(先还欠费)").arg(topUp));
        db.getUserById(poor.userId, &poor);
        CHECK(qAbs(repay - poorDone.debt) < 0.001 && remain <= 0.001
                  && poor.debt <= 0.001 && qAbs(poor.balance - 5.0) < 0.001,
              QStringLiteral("还款 %1 元, 剩余欠费 %2, 余额 %3")
                  .arg(repay, 0, 'f', 2).arg(poor.debt, 0, 'f', 2).arg(poor.balance, 0, 'f', 2));
        CHECK(db.orderConnect(poor.userId, fast.chargerId, &poorOrder, &e),
              QStringLiteral("还清欠费后可以再次下单"));
        db.orderCancel(poorOrder, &e);   // 清理
    }

    // ---------------- 统计 ----------------
    DBManager::RevenueSummary rev;
    QString serr;
    CHECK(db.revenueSummary(&rev, &serr), QStringLiteral("营收汇总可查"));
    DBManager::ChargerStatusCount sc;
    CHECK(db.chargerStatusCount(&sc, &serr) && sc.total == rowCount(db, QStringLiteral("charger")),
          QStringLiteral("电桩状态统计与总数一致"));

    // ---------------- 运维日志 ----------------
    CHECK(db.addOpsLog(QStringLiteral("admin"), idle.chargerId, after.code,
                       QStringLiteral("测试动作")),
          QStringLiteral("写入运维日志"));
    QVector<DBManager::OpsLog> logs;
    CHECK(db.listOpsLogs(10, &logs, &serr) && !logs.isEmpty(),
          QStringLiteral("查询运维日志 %1 条").arg(logs.size()));

    // ---------------- 设备接入(Charger Simulator 对接, v7) ----------------
    // 造一台"干净"的桩给设备层测试: 新建测试站 + 单桩
    qint64 devStationId = 0;
    CHECK(db.addStation(QStringLiteral("设备测试电站"), QStringLiteral("DEV"),
                        QStringLiteral("测试地址"), 123.0, 41.0, 1.0, &devStationId, &serr),
          QStringLiteral("设备测试: 新建电站"));
    CHECK(db.addCharger(devStationId, QStringLiteral("DEV-01"), 1, 60.0, &serr),
          QStringLiteral("设备测试: 新建电桩 DEV-01"));
    DBManager::Charger dev;
    for (const DBManager::Charger &c : db.listChargers(devStationId)) {
        if (c.code == QStringLiteral("DEV-01")) { dev = c; break; }
    }
    CHECK(dev.chargerId != 0, QStringLiteral("设备测试: 找到 DEV-01"));

    // 遥测: 追加写入 + 按桩裁剪旧帧
    DBManager::Telemetry tm;
    tm.chargerId = dev.chargerId;
    tm.ts = QStringLiteral("2026-09-07 10:00:00");
    tm.status = QStringLiteral("charging");
    tm.power = 60.0;
    tm.soc = 33.3;
    tm.energy = 1.25;
    tm.temperature = 31.5;
    CHECK(db.insertTelemetry(tm, &serr), QStringLiteral("遥测帧写入"));
    DBManager::Telemetry tm2 = tm;
    tm2.ts = QStringLiteral("2026-09-07 10:00:01");
    tm2.soc = 33.8;
    db.insertTelemetry(tm2, &serr);
    CHECK(rowCount(db, QStringLiteral("charger_telemetry")) == 2,
          QStringLiteral("遥测追加为 2 帧"));
    CHECK(db.trimTelemetry(dev.chargerId, 1, &serr), QStringLiteral("遥测裁剪执行"));
    CHECK(rowCount(db, QStringLiteral("charger_telemetry")) == 1,
          QStringLiteral("裁剪后每桩只留最新帧"));

    // 心跳: 每桩一行 upsert
    DBManager::Heartbeat hb;
    hb.chargerId = dev.chargerId;
    hb.lastSeen = QStringLiteral("2026-09-07 10:00:05");
    hb.status = QStringLiteral("charging");
    hb.uptimeS = 125;
    CHECK(db.upsertHeartbeat(hb, &serr), QStringLiteral("心跳首写"));
    hb.lastSeen = QStringLiteral("2026-09-07 10:00:10");
    hb.uptimeS = 130;
    CHECK(db.upsertHeartbeat(hb, &serr), QStringLiteral("心跳再次上报(upsert)"));
    CHECK(rowCount(db, QStringLiteral("charger_heartbeat")) == 1,
          QStringLiteral("心跳每桩只有一行"));
    DBManager::Heartbeat hbOut;
    CHECK(db.getHeartbeat(dev.chargerId, &hbOut) && hbOut.uptimeS == 130
              && hbOut.lastSeen == QStringLiteral("2026-09-07 10:00:10"),
          QStringLiteral("心跳读取为最新值"));
    QVector<DBManager::Heartbeat> hbs;
    CHECK(db.listHeartbeats(&hbs, &serr) && hbs.size() == 1,
          QStringLiteral("心跳列表可查"));

    // 状态投影: 有进行中订单时跳过(订单流程负责), 无订单时设备状态写入
    qint64 devOrderId = 0;
    CHECK(db.orderConnect(user4.userId, dev.chargerId, &devOrderId, &serr),
          QStringLiteral("设备测试: 给 DEV-01 下单(桩转已连接)"));
    CHECK(db.syncChargerDeviceStatus(dev.chargerId, ChargerIdle, &serr),
          QStringLiteral("订单中状态投影不报错(跳过)"));
    DBManager::Charger proj;
    db.getCharger(dev.chargerId, &proj);
    CHECK(proj.status == ChargerConnected,
          QStringLiteral("有进行中订单时设备上报不覆盖订单状态"));
    db.orderCancel(devOrderId, &serr);
    CHECK(db.syncChargerDeviceStatus(dev.chargerId, ChargerCharging, &serr),
          QStringLiteral("无订单时状态投影执行"));
    db.getCharger(dev.chargerId, &proj);
    CHECK(proj.status == ChargerCharging,
          QStringLiteral("设备状态已投影 charger.status"));
    db.syncChargerDeviceStatus(dev.chargerId, ChargerIdle, &serr);   // 还原空闲
    CHECK(!db.syncChargerDeviceStatus(dev.chargerId, 9, &serr),
          QStringLiteral("非法状态投影被拒"));

    // 命令通道: 平台下发 -> 设备领取 -> 回填结果
    qint64 cmdId = 0;
    CHECK(db.pushDeviceCommand(dev.chargerId, QStringLiteral("start"), QString(),
                               &cmdId, &serr) && cmdId != 0,
          QStringLiteral("平台下发设备命令"));
    CHECK(!db.pushDeviceCommand(999999, QStringLiteral("start"), QString(), nullptr, &serr),
          QStringLiteral("给不存在的桩下发被拒: %1").arg(serr));
    QVector<DBManager::DeviceCommand> pending;
    CHECK(db.takePendingDeviceCommands(dev.chargerId, &pending, &serr)
              && pending.size() == 1 && pending.at(0).command == QStringLiteral("start"),
          QStringLiteral("设备领取到待执行命令"));
    CHECK(db.finishDeviceCommand(cmdId, true, QStringLiteral("ok"), &serr),
          QStringLiteral("设备回填执行成功"));
    CHECK(db.takePendingDeviceCommands(dev.chargerId, &pending, &serr) && pending.isEmpty(),
          QStringLiteral("执行后命令不再被领取"));
    qint64 cmdId2 = 0;
    db.pushDeviceCommand(dev.chargerId, QStringLiteral("fault"), QStringLiteral("2"),
                         &cmdId2, &serr);
    CHECK(db.finishDeviceCommand(cmdId2, false, QStringLiteral("需要状态 reserved"), &serr),
          QStringLiteral("设备回填执行失败结果"));
    QVector<DBManager::DeviceCommand> cmdList;
    CHECK(db.listDeviceCommands(10, &cmdList, &serr) && cmdList.size() == 2
              && cmdList.at(0).status == 2,
          QStringLiteral("命令历史可查(含失败状态)"));

    qInfo().noquote() << QStringLiteral("\n结果: 通过 %1, 失败 %2").arg(g_pass).arg(g_fail);
    return g_fail == 0 ? 0 : 1;
}

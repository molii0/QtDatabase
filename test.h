#ifndef TEST_H
#define TEST_H

// ============================================================================
// 控制台功能测试(main 演示模式调用)
// 测试会修改数据库(注册/充值/下单/取消/结算), 请用测试库运行。
// ============================================================================

#include "DBManager.h"

#include <QDateTime>
#include <QDebug>
#include <QSqlQuery>
#include <QVector>

// 打印一张表有多少行
static void printTableCount(DBManager &db, const QString &table)
{
    QSqlQuery q(db.db());
    q.exec(QStringLiteral("SELECT COUNT(*) FROM %1;").arg(table));
    q.next();
    qInfo().noquote() << QStringLiteral("    %1 表: %2 行").arg(table).arg(q.value(0).toInt());
}

static void demoUser(DBManager &db)
{
    qInfo() << "\n--- 用户操作(user) ---";

    // 1. 按手机号查已有用户(登录时用)
    DBManager::User u;
    const bool found = db.getUserByPhone(QStringLiteral("12345678910"), &u);
    qInfo().noquote() << QStringLiteral("查手机号 12345678910:")
                      << (found ? QStringLiteral("找到 %1, 余额 %2 元")
                                      .arg(u.nickname).arg(u.balance, 0, 'f', 2)
                                : QStringLiteral("不存在"));

    // 2. 注册一个新用户(手机号不存在 -> 自动注册)
    const bool regOk = db.insertUser(QStringLiteral("13800001234"), QStringLiteral("新用户"));
    qInfo().noquote() << QStringLiteral("注册 13800001234:") << (regOk ? "成功" : "失败");

    // 3. 重复手机号再注册 -> 应失败
    const bool dupOk = db.insertUser(QStringLiteral("13800001234"), QStringLiteral("重复注册"));
    qInfo().noquote() << QStringLiteral("重复手机号再注册:") << (dupOk ? "成功(异常)" : "被拒绝(正确)");

    // 4. 给新用户充值 30 元, 再查一次余额
    if (db.getUserByPhone(QStringLiteral("13800001234"), &u))
        db.updateBalance(u.userId, 30.0);
    db.getUserByPhone(QStringLiteral("13800001234"), &u);
    qInfo().noquote() << QStringLiteral("新用户 %1 充值后余额: %2 元")
                             .arg(u.nickname).arg(u.balance, 0, 'f', 2);
}

static void demoAdmin(DBManager &db)
{
    qInfo() << "\n--- 管理员登录(admin) ---";
    qInfo().noquote() << QStringLiteral("admin / 123456:")
                      << (db.checkAdminLogin(QStringLiteral("admin"), QStringLiteral("123456"))
                              ? "登录成功"
                              : "登录失败");
    qInfo().noquote() << QStringLiteral("admin / 错误密码:")
                      << (db.checkAdminLogin(QStringLiteral("admin"), QStringLiteral("000000"))
                              ? "登录成功(异常)"
                              : "被拒绝(正确)");
}

static void demoStation(DBManager &db)
{
    qInfo() << "\n--- 充电站与充电桩(station / charger) ---";

    const QVector<DBManager::Station> stations = db.listStations();
    qInfo().noquote() << QStringLiteral("共有 %1 座充电站:").arg(stations.size());
    for (const DBManager::Station &s : stations) {
        const QVector<DBManager::Charger> chargers = db.listChargers(s.stationId);
        int idle = 0, connected = 0, charging = 0, fault = 0;
        for (const DBManager::Charger &c : chargers) {
            if (c.status == ChargerIdle)           ++idle;
            else if (c.status == ChargerConnected) ++connected;
            else if (c.status == ChargerCharging)  ++charging;
            else if (c.status == ChargerFault)     ++fault;
        }
        qInfo().noquote() << QStringLiteral("    %1: %2 元/度 | 空闲 %3 / 已连接 %4 / 充电中 %5 / 故障 %6")
                                 .arg(s.name)
                                 .arg(s.price, 0, 'f', 2)
                                 .arg(idle).arg(connected).arg(charging).arg(fault);
    }

    // 演示: 远程重启一台故障桩(状态机: 故障 -> 空闲)
    const QVector<DBManager::Charger> all = db.listChargers();
    for (const DBManager::Charger &c : all) {
        if (c.status == ChargerFault) {
            QString err;
            const bool ok = db.adminRecover(c.chargerId, &err);
            qInfo().noquote() << QStringLiteral("远程重启故障桩 %1:").arg(c.code)
                              << (ok ? "成功" : QStringLiteral("失败: %1").arg(err));
            break;
        }
    }
}

static void demoStationManage(DBManager &db)
{
    qInfo() << "\n--- 电站/桩管理: 新增/删除/批量编号 ---";

    // 1. 新增一座电站(缩写 SY)
    qint64 newStationId = 0;
    QString err;
    if (!db.addStation(QStringLiteral("演示测试站"), QStringLiteral("SY"),
                       QStringLiteral("某市某区测试路1号"), 123.40, 41.80, 1.10,
                       &newStationId, &err)) {
        qWarning() << "新增电站失败:" << err;
        return;
    }
    qInfo().noquote() << QStringLiteral("新增电站成功 (station_id=%1)").arg(newStationId);

    // 2. 批量建 3 台慢充桩, 编号应从 SY-01 自动顺延
    int created = 0;
    const bool batchOk = db.batchAddChargers(newStationId, 3, 0, 7.0, &created, &err);
    qInfo().noquote() << QStringLiteral("批量建桩 %1 台: %2").arg(created)
                             .arg(batchOk ? "成功" : QStringLiteral("失败: %1").arg(err));
    const QVector<DBManager::Charger> after = db.listChargers(newStationId);
    for (const DBManager::Charger &c : after)
        qInfo().noquote() << QStringLiteral("    桩编号: %1 (功率 %2 kW)").arg(c.code).arg(c.power);

    // 3. 删除"仍有电桩"的电站 -> 应被拒绝(BR-10)
    err.clear();
    const bool delStationBlocked = !db.deleteStation(newStationId, &err);
    qInfo().noquote() << QStringLiteral("删除仍有桩的电站:") << (delStationBlocked ? "被拒绝(正确)" : "被删除(异常)");

    // 4. 清理: 删掉刚建的电桩, 再删电站
    for (const DBManager::Charger &c : after) {
        err.clear();
        db.deleteCharger(c.chargerId, &err);
    }
    err.clear();
    const bool delStationOk = db.deleteStation(newStationId, &err);
    qInfo().noquote() << QStringLiteral("清空桩后删除电站:") << (delStationOk ? "成功" : QStringLiteral("失败: %1").arg(err));
}

// 订单流程演示(状态机 + BR-02): 结算种子充电订单 -> 下单[待支付] -> 重复下单被拒 -> 取消
static void demoOrderFlow(DBManager &db)
{
    qInfo() << "\n--- 订单流程(orderConnect/orderStart/orderCancel/orderFinish) ---";

    DBManager::User u;
    if (!db.getUserByPhone(QStringLiteral("12345678910"), &u))
        return;

    // B. 先结算种子里的"充电中"订单(15 分钟前开始, 有真实时长可算钱),
    //    结算后用户才有"未结算订单=无"的状态, 便于演示下一步下单
    {
        const QVector<DBManager::Order> mine = db.listOrders(u.userId);
        bool foundCharging = false;
        for (const DBManager::Order &o : mine) {
            if (o.status == OrderCharging) {
                foundCharging = true;
                QString err3;
                const bool ok = db.orderFinish(o.orderId, &err3);
                qInfo().noquote() << QStringLiteral("结算种子充电中订单 %1: %2")
                                         .arg(o.orderNo)
                                         .arg(ok ? QStringLiteral("成功") : QStringLiteral("失败: %1").arg(err3));
                if (ok) {
                    DBManager::Order done;
                    db.getOrderById(o.orderId, &done);
                    qInfo().noquote() << QStringLiteral("  电量 %1 度 | 费用 %2 元 | 结束时间 %3")
                                             .arg(done.energy, 0, 'f', 2)
                                             .arg(done.amount, 0, 'f', 2)
                                             .arg(done.endTime);
                }
                break;
            }
        }
        if (!foundCharging)
            qInfo().noquote() << QStringLiteral("没有充电中的种子订单可结算(可能上回已结算)");
    }

    // A. 用户"测试用户"选一台空闲桩下单 -> 订单[待支付], 电桩[已连接]
    DBManager::Charger idle;
    const QVector<DBManager::Charger> all = db.listChargers();
    for (const DBManager::Charger &c : all) {
        if (c.status == ChargerIdle) { idle = c; break; }
    }
    if (idle.chargerId == 0) {
        qWarning() << "没有空闲桩, 跳过下单演示";
        return;
    }

    QString err;
    qint64 orderId = 0;
    const bool okConnect = db.orderConnect(u.userId, idle.chargerId, &orderId, &err);
    qInfo().noquote() << QStringLiteral("选桩下单(%1):").arg(idle.code) << (okConnect ? "成功" : "失败: " + err);
    if (okConnect) {
        DBManager::Order o;
        db.getOrderById(orderId, &o);
        qInfo().noquote() << QStringLiteral("  订单状态: %1").arg(orderStateText(o.status));

        // BR-02: 同一用户已有未结算订单时, 再下单会被拒绝
        QString errBr;
        const bool dup = db.orderConnect(u.userId, idle.chargerId, nullptr, &errBr);
        qInfo().noquote() << QStringLiteral("  未结算时重复下单:") << (dup ? "成功(异常)" : QStringLiteral("被拒绝(正确): %1").arg(errBr));

        // 非法转换演示: 还没开始充电就"结算" -> 应被状态机拒绝
        QString err2;
        const bool bad = db.orderFinish(orderId, &err2);
        qInfo().noquote() << QStringLiteral("  未充电直接结算:") << (bad ? "成功(异常)" : QStringLiteral("被拒绝(正确): %1").arg(err2));

        // 取消: 订单[待支付->已取消], 电桩[已连接->空闲]
        const bool okCancel = db.orderCancel(orderId, &err2);
        qInfo().noquote() << QStringLiteral("  取消订单:") << (okCancel ? "成功, 电桩已释放为空闲" : "失败: " + err2);
    }
}

static void demoStats(DBManager &db)
{
    qInfo() << "\n--- 统计(管理端用) ---";
    DBManager::RevenueSummary rev;
    QString err;
    if (db.revenueSummary(&rev, &err))
        qInfo().noquote() << QStringLiteral("营收: 今日 %1 元 | 本月 %2 元 | 总计 %3 元")
                                 .arg(rev.today, 0, 'f', 2).arg(rev.month, 0, 'f', 2).arg(rev.total, 0, 'f', 2);
    DBManager::ChargerStatusCount c;
    if (db.chargerStatusCount(&c, &err))
        qInfo().noquote() << QStringLiteral("电桩: 空闲 %1 | 已连接 %2 | 充电中 %3 | 故障 %4 | 共 %5")
                                 .arg(c.idle).arg(c.connected).arg(c.charging).arg(c.fault).arg(c.total);
}

#endif

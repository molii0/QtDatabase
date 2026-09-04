#include "DBManager.h"

#include <QDateTime>
#include <QDebug>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>

// ============================================================================
// 订单流程(charging_order)
//   每个改状态的方法都先查"状态机"(ChargeState.h)确认转换合法, 再在事务里写库。
//   流程:
//     orderConnect  选桩下单 -> 订单[待支付], 电桩[已连接]
//     orderStart    开始充电 -> 订单[充电中], 电桩[充电中], 写 start_time
//     orderCancel   取消     -> 订单[已取消], 电桩[空闲](只允许未开始的订单)
//     orderFinish   结束结算 -> 订单[已完成], 扣余额, 电桩[空闲]
// ============================================================================

QString DBManager::makeOrderNo()
{
    // 时间戳 + 4 位随机数, 保证并发下也不重号
    return QStringLiteral("N%1%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QRandomGenerator::global()->bounded(10000), 4, 10, QLatin1Char('0'));
}

bool DBManager::getOrderById(qint64 orderId, Order *out) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT order_id, order_no, user_id, station_id, charger_id, status,"
        "       energy, amount, start_time, end_time"
        " FROM charging_order WHERE order_id = :id;"));
    q.bindValue(QStringLiteral(":id"), orderId);
    if (!q.exec() || !q.next())
        return false;
    if (out) {
        out->orderId = q.value(0).toLongLong();
        out->orderNo = q.value(1).toString();
        out->userId = q.value(2).toLongLong();
        out->stationId = q.value(3).toLongLong();
        out->chargerId = q.value(4).toLongLong();
        out->status = q.value(5).toInt();
        out->energy = q.value(6).toDouble();
        out->amount = q.value(7).toDouble();
        out->startTime = q.value(8).toString();
        out->endTime = q.value(9).toString();
    }
    return true;
}

QVector<DBManager::Order> DBManager::listOrders(qint64 userId) const
{
    QVector<Order> result;
    QSqlQuery q(db());
    if (userId < 0) {
        q.exec(QStringLiteral(
            "SELECT order_id, order_no, user_id, station_id, charger_id, status,"
            "       energy, amount, start_time, end_time"
            " FROM charging_order ORDER BY order_id DESC;"));
    } else {
        // idx_order_user / idx_order_user_status 索引加速该查询
        q.prepare(QStringLiteral(
            "SELECT order_id, order_no, user_id, station_id, charger_id, status,"
            "       energy, amount, start_time, end_time"
            " FROM charging_order WHERE user_id = :id ORDER BY order_id DESC;"));
        q.bindValue(QStringLiteral(":id"), userId);
        q.exec();
    }
    while (q.next()) {
        Order o;
        o.orderId = q.value(0).toLongLong();
        o.orderNo = q.value(1).toString();
        o.userId = q.value(2).toLongLong();
        o.stationId = q.value(3).toLongLong();
        o.chargerId = q.value(4).toLongLong();
        o.status = q.value(5).toInt();
        o.energy = q.value(6).toDouble();
        o.amount = q.value(7).toDouble();
        o.startTime = q.value(8).toString();
        o.endTime = q.value(9).toString();
        result.append(o);
    }
    return result;
}

// ---- 状态机校验 + 带守卫的状态更新工具(都在同一个事务里用) ----

static bool chargerSetState(const QSqlDatabase &dbc, qint64 chargerId, int from, int to, QString *err)
{
    QString why;
    if (!chargerCanTransition(from, to, &why)) {
        if (err) *err = why;
        return false;
    }
    QSqlQuery q(dbc);
    q.prepare(QStringLiteral(
        "UPDATE charger SET status = :to WHERE charger_id = :id AND status = :from;"));
    q.bindValue(QStringLiteral(":to"), to);
    q.bindValue(QStringLiteral(":id"), chargerId);
    q.bindValue(QStringLiteral(":from"), from);
    if (!q.exec() || q.numRowsAffected() != 1) {
        if (err) *err = QStringLiteral("电桩状态已变化(可能被并发修改)，请刷新后重试");
        return false;
    }
    return true;
}

static bool orderSetState(const QSqlDatabase &dbc, qint64 orderId, int from, int to, QString *err)
{
    QString why;
    if (!orderCanTransition(from, to, &why)) {
        if (err) *err = why;
        return false;
    }
    QSqlQuery q(dbc);
    q.prepare(QStringLiteral(
        "UPDATE charging_order SET status = :to WHERE order_id = :id AND status = :from;"));
    q.bindValue(QStringLiteral(":to"), to);
    q.bindValue(QStringLiteral(":id"), orderId);
    q.bindValue(QStringLiteral(":from"), from);
    if (!q.exec() || q.numRowsAffected() != 1) {
        if (err) *err = QStringLiteral("订单状态已变化(可能被并发修改)，请刷新后重试");
        return false;
    }
    return true;
}

// ---------------- 1. 选桩下单: 订单[待支付], 电桩[空闲->已连接] ----------------
bool DBManager::orderConnect(qint64 userId, qint64 chargerId, qint64 *newOrderId, QString *err)
{
    if (err)
        err->clear();
    if (!beginTransaction()) {
        if (err) *err = QStringLiteral("开启事务失败");
        return false;
    }
    const auto rollbackAndFail = [&](const QString &msg) {
        if (err) *err = msg;
        rollbackTransaction();
        return false;
    };
    const QSqlDatabase dbc = db();

    // 用户校验(存在 / 未冻结)
    QSqlQuery q(dbc);
    q.prepare(QStringLiteral("SELECT status FROM user WHERE user_id = :id;"));
    q.bindValue(QStringLiteral(":id"), userId);
    if (!q.exec() || !q.next())
        return rollbackAndFail(QStringLiteral("用户不存在 (userId=%1)").arg(userId));
    if (q.value(0).toInt() == 0)
        return rollbackAndFail(QStringLiteral("账号已被冻结，不能下单充电"));

    // 电桩校验(存在 / 空闲)
    q.prepare(QStringLiteral("SELECT station_id, status, code FROM charger WHERE charger_id = :id;"));
    q.bindValue(QStringLiteral(":id"), chargerId);
    if (!q.exec() || !q.next())
        return rollbackAndFail(QStringLiteral("电桩不存在 (chargerId=%1)").arg(chargerId));
    const qint64 stationId = q.value(0).toLongLong();
    const int chargerStatus = q.value(1).toInt();
    const QString code = q.value(2).toString();
    if (chargerStatus != ChargerIdle)
        return rollbackAndFail(QStringLiteral("电桩 %1 当前为[%2]，请选择空闲电桩")
                                   .arg(code, chargerStateText(chargerStatus)));

    // 插入"待支付"订单
    q.prepare(QStringLiteral(
        "INSERT INTO charging_order (order_no, user_id, station_id, charger_id, status)"
        " VALUES (:no, :uid, :sid, :cid, 0);"));
    q.bindValue(QStringLiteral(":no"), makeOrderNo());
    q.bindValue(QStringLiteral(":uid"), userId);
    q.bindValue(QStringLiteral(":sid"), stationId);
    q.bindValue(QStringLiteral(":cid"), chargerId);
    if (!q.exec())
        return rollbackAndFail(QStringLiteral("创建订单失败: %1").arg(q.lastError().text()));
    const qint64 orderId = q.lastInsertId().toLongLong();

    // 电桩 空闲 -> 已连接
    QString chErr;
    if (!chargerSetState(dbc, chargerId, ChargerIdle, ChargerConnected, &chErr)) {
        rollbackTransaction();
        if (err) *err = chErr;
        return false;
    }

    if (!commitTransaction()) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("提交事务失败"));
    }
    if (newOrderId)
        *newOrderId = orderId;
    qDebug() << "选桩下单: 订单" << orderId << ", 用户" << userId << ", 电桩" << chargerId << "(已连接)";
    return true;
}

// ---------------- 2. 开始充电: 订单[待支付->充电中], 电桩[已连接->充电中] ----------------
bool DBManager::orderStart(qint64 orderId, QString *err)
{
    if (err)
        err->clear();
    if (!beginTransaction()) {
        if (err) *err = QStringLiteral("开启事务失败");
        return false;
    }
    const auto rollbackAndFail = [&](const QString &msg) {
        if (err) *err = msg;
        rollbackTransaction();
        return false;
    };
    const QSqlDatabase dbc = db();

    QSqlQuery q(dbc);
    q.prepare(QStringLiteral("SELECT charger_id, status FROM charging_order WHERE order_id = :id;"));
    q.bindValue(QStringLiteral(":id"), orderId);
    if (!q.exec() || !q.next())
        return rollbackAndFail(QStringLiteral("订单不存在 (orderId=%1)").arg(orderId));
    const qint64 chargerId = q.value(0).toLongLong();
    const int orderStatus = q.value(1).toInt();

    // 订单必须是"待支付"才能开始充电
    if (orderStatus != OrderWaitPay)
        return rollbackAndFail(QStringLiteral("订单当前为[%1]，不能开始充电")
                                   .arg(orderStateText(orderStatus)));

    // 订单 待支付 -> 充电中, 并写 start_time
    QString stErr;
    if (!orderSetState(dbc, orderId, OrderWaitPay, OrderCharging, &stErr)) {
        rollbackTransaction();
        if (err) *err = stErr;
        return false;
    }
    q.prepare(QStringLiteral("UPDATE charging_order SET start_time = :t WHERE order_id = :id;"));
    q.bindValue(QStringLiteral(":t"), nowStr());
    q.bindValue(QStringLiteral(":id"), orderId);
    if (!q.exec()) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("写入开始时间失败"));
    }

    // 电桩 已连接 -> 充电中
    if (!chargerSetState(dbc, chargerId, ChargerConnected, ChargerCharging, &stErr)) {
        rollbackTransaction();
        if (err) *err = stErr;
        return false;
    }

    if (!commitTransaction()) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("提交事务失败"));
    }
    qDebug() << "开始充电: 订单" << orderId << "电桩" << chargerId << "已进入充电中";
    return true;
}

// ---------------- 3. 取消: 订单[待支付->已取消], 电桩[已连接->空闲] ----------------
bool DBManager::orderCancel(qint64 orderId, QString *err)
{
    if (err)
        err->clear();
    if (!beginTransaction()) {
        if (err) *err = QStringLiteral("开启事务失败");
        return false;
    }
    const auto rollbackAndFail = [&](const QString &msg) {
        if (err) *err = msg;
        rollbackTransaction();
        return false;
    };
    const QSqlDatabase dbc = db();

    QSqlQuery q(dbc);
    q.prepare(QStringLiteral("SELECT charger_id, status FROM charging_order WHERE order_id = :id;"));
    q.bindValue(QStringLiteral(":id"), orderId);
    if (!q.exec() || !q.next())
        return rollbackAndFail(QStringLiteral("订单不存在 (orderId=%1)").arg(orderId));
    const qint64 chargerId = q.value(0).toLongLong();
    const int orderStatus = q.value(1).toInt();

    if (orderStatus == OrderFinished || orderStatus == OrderCanceled)
        return rollbackAndFail(QStringLiteral("订单已[%1]，不能取消").arg(orderStateText(orderStatus)));
    if (orderStatus != OrderWaitPay)
        return rollbackAndFail(QStringLiteral("订单正在充电中，不能取消，请先结算"));

    QString stErr;
    if (!orderSetState(dbc, orderId, OrderWaitPay, OrderCanceled, &stErr)) {
        rollbackTransaction();
        if (err) *err = stErr;
        return false;
    }
    if (!chargerSetState(dbc, chargerId, ChargerConnected, ChargerIdle, &stErr)) {
        rollbackTransaction();
        if (err) *err = stErr;
        return false;
    }

    if (!commitTransaction()) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("提交事务失败"));
    }
    qDebug() << "取消订单:" << orderId;
    return true;
}

// ---------------- 4. 结束充电并结算(事务) ----------------
// 按实际充电时长算费用 -> 校验余额 -> 扣款 -> 订单[充电中->已完成] -> 电桩[充电中->空闲]
bool DBManager::orderFinish(qint64 orderId, QString *err)
{
    if (err)
        err->clear();
    if (!beginTransaction()) {
        if (err) *err = QStringLiteral("开启事务失败");
        return false;
    }
    const auto rollbackAndFail = [&](const QString &msg) {
        if (err) *err = msg;
        rollbackTransaction();
        return false;
    };
    const QSqlDatabase dbc = db();

    // 1. 读取订单及其关联的电桩功率、电站单价(只处理"充电中"的订单)
    QSqlQuery query(dbc);
    query.prepare(QStringLiteral(
        "SELECT o.user_id, o.charger_id, o.start_time, c.power, s.price, o.status"
        "  FROM charging_order o"
        "  JOIN charger c ON o.charger_id = c.charger_id"
        "  JOIN station s ON o.station_id = s.station_id"
        " WHERE o.order_id = :id;"));
    query.bindValue(QStringLiteral(":id"), orderId);
    if (!query.exec() || !query.next())
        return rollbackAndFail(QStringLiteral("订单不存在 (orderId=%1)").arg(orderId));

    if (query.value(5).toInt() != OrderCharging)
        return rollbackAndFail(QStringLiteral("订单当前为[%1]，不能结算")
                                   .arg(orderStateText(query.value(5).toInt())));

    const qint64 userId = query.value(0).toLongLong();
    const qint64 chargerId = query.value(1).toLongLong();
    const QDateTime startTime = QDateTime::fromString(query.value(2).toString(),
                                                      QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const double power = query.value(3).toDouble();   // 电桩功率(kW)
    const double price = query.value(4).toDouble();   // 电站单价(元/度)
    if (!startTime.isValid())
        return rollbackAndFail(QStringLiteral("订单开始时间格式无效"));

    // 2. 按已充电时长计算电量与费用: 电量 = 功率×小时, 费用 = 电量×单价
    const qint64 durationSecs = startTime.secsTo(QDateTime::currentDateTime());
    if (durationSecs <= 0)
        return rollbackAndFail(QStringLiteral("充电时长不足(≤0 秒)，无法结算，请稍后再试"));

    const double energy = round2(power * durationSecs / 3600.0);
    double amount = round2(energy * price);
    if (amount < 0.01 && amount > 0)
        amount = 0.01;      // 极小金额保护, 防止"免费充电"
    qDebug() << "结算: 时长" << durationSecs / 60 << "分钟,"
             << "电量" << energy << "度, 费用" << amount << "元";

    // 3. 校验余额是否够扣(不够就整体回滚, 提示先充值)
    QSqlQuery balanceQuery(dbc);
    balanceQuery.prepare(QStringLiteral("SELECT balance FROM user WHERE user_id = :uid;"));
    balanceQuery.bindValue(QStringLiteral(":uid"), userId);
    if (!balanceQuery.exec() || !balanceQuery.next())
        return rollbackAndFail(QStringLiteral("查询用户余额失败"));
    const double userBalance = balanceQuery.value(0).toDouble();
    if (userBalance < amount)
        return rollbackAndFail(QStringLiteral("用户余额不足(当前 %1 元, 需 %2 元)，请先充值")
                                   .arg(userBalance, 0, 'f', 2)
                                   .arg(amount, 0, 'f', 2));

    // 4. 订单 充电中 -> 已完成, 写入电量/费用/结束时间
    QString stErr;
    if (!orderSetState(dbc, orderId, OrderCharging, OrderFinished, &stErr)) {
        rollbackTransaction();
        if (err) *err = stErr;
        return false;
    }
    QSqlQuery updateOrder(dbc);
    updateOrder.prepare(QStringLiteral(
        "UPDATE charging_order SET energy = :en, amount = :am, end_time = :et"
        " WHERE order_id = :id AND status = 2;"));
    updateOrder.bindValue(QStringLiteral(":en"), energy);
    updateOrder.bindValue(QStringLiteral(":am"), amount);
    updateOrder.bindValue(QStringLiteral(":et"), nowStr());
    updateOrder.bindValue(QStringLiteral(":id"), orderId);
    if (!updateOrder.exec()) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("写入订单结果失败: %1").arg(updateOrder.lastError().text()));
    }

    // 5. 扣用户余额
    QSqlQuery updateUser(dbc);
    updateUser.prepare(QStringLiteral(
        "UPDATE user SET balance = balance - :am WHERE user_id = :uid AND balance >= :am;"));
    updateUser.bindValue(QStringLiteral(":am"), amount);
    updateUser.bindValue(QStringLiteral(":uid"), userId);
    if (!updateUser.exec() || updateUser.numRowsAffected() != 1) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("扣减用户余额失败"));
    }

    // 6. 电桩 充电中 -> 空闲
    if (!chargerSetState(dbc, chargerId, ChargerCharging, ChargerIdle, &stErr)) {
        rollbackTransaction();
        if (err) *err = stErr;
        return false;
    }

    if (!commitTransaction()) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("提交事务失败"));
    }
    qDebug() << "订单" << orderId << "结算成功, 消费" << amount << "元";
    return true;
}

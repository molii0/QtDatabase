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

// 订单查询公共列(JION 出站名/桩号/手机号, 供列表与小票展示)
static const char kOrderCols[] =
    "SELECT o.order_id, o.order_no, o.user_id, o.station_id, o.charger_id, o.status,"
    "       o.energy, o.amount, o.paid, o.debt, o.start_time, o.end_time,"
    "       s.name, c.code, u.phone"
    "  FROM charging_order o"
    "  JOIN station s ON s.station_id = o.station_id"
    "  JOIN charger c ON c.charger_id = o.charger_id"
    "  JOIN \"user\" u ON u.user_id = o.user_id ";

static void fillOrder(QSqlQuery &q, DBManager::Order &o)
{
    o.orderId = q.value(0).toLongLong();
    o.orderNo = q.value(1).toString();
    o.userId = q.value(2).toLongLong();
    o.stationId = q.value(3).toLongLong();
    o.chargerId = q.value(4).toLongLong();
    o.status = q.value(5).toInt();
    o.energy = q.value(6).toDouble();
    o.amount = q.value(7).toDouble();
    o.paid = q.value(8).toDouble();
    o.debt = q.value(9).toDouble();
    o.startTime = q.value(10).toString();
    o.endTime = q.value(11).toString();
    o.stationName = q.value(12).toString();
    o.chargerCode = q.value(13).toString();
    o.userPhone = q.value(14).toString();
}

bool DBManager::getOrderById(qint64 orderId, Order *out) const
{
    QSqlQuery q(db());
    q.prepare(QString::fromLatin1(kOrderCols) + QStringLiteral("WHERE o.order_id = :id;"));
    q.bindValue(QStringLiteral(":id"), orderId);
    if (!q.exec() || !q.next())
        return false;
    if (out)
        fillOrder(q, *out);
    return true;
}

// 当前用户未结算订单(待支付0/充电中1); 无则返回 false(不视为错误)
bool DBManager::getActiveOrderOfUser(qint64 userId, Order *out, QString *err) const
{
    QSqlQuery q(db());
    q.prepare(QString::fromLatin1(kOrderCols)
              + QStringLiteral("WHERE o.user_id = :id AND o.status IN (0, 1) "
                               "ORDER BY o.order_id DESC LIMIT 1;"));
    q.bindValue(QStringLiteral(":id"), userId);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("查询未结算订单失败: %1").arg(q.lastError().text());
        return false;
    }
    if (!q.next())
        return false;   // 没有未结算订单
    if (out)
        fillOrder(q, *out);
    return true;
}

QVector<DBManager::Order> DBManager::listOrders(qint64 userId) const
{
    QVector<Order> result;
    QSqlQuery q(db());
    if (userId < 0) {
        q.exec(QString::fromLatin1(kOrderCols)
               + QStringLiteral("ORDER BY o.order_id DESC;"));
    } else {
        // idx_order_user / idx_order_user_status 索引加速该查询
        q.prepare(QString::fromLatin1(kOrderCols)
                  + QStringLiteral("WHERE o.user_id = :id ORDER BY o.order_id DESC;"));
        q.bindValue(QStringLiteral(":id"), userId);
        q.exec();
    }
    if (q.lastError().isValid())
        return result;
    while (q.next()) {
        Order o;
        fillOrder(q, o);
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

    // BR-02: 同一用户已有未结算订单(待支付0/充电中1)时不允许再下单
    q.prepare(QStringLiteral(
        "SELECT order_id FROM charging_order WHERE user_id = :id AND status IN (0, 1) LIMIT 1;"));
    q.bindValue(QStringLiteral(":id"), userId);
    if (q.exec() && q.next())
        return rollbackAndFail(QStringLiteral("您有未完成的充电订单，请先结算或取消后再下单"));

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
    if (!q.exec()) {
        // 数据库级 BR-02/BR-03 唯一索引兜底(并发抢单时的最后防线)
        const QString msg = q.lastError().text();
        if (msg.contains(QStringLiteral("uq_order_")))
            return rollbackAndFail(QStringLiteral("操作冲突：您有未完成订单或该电桩已被占用，请刷新后重试"));
        return rollbackAndFail(QStringLiteral("创建订单失败: %1").arg(msg));
    }
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
// 按实际充电时长算费用 -> 读余额算"实扣/欠费"(BR-06: 不足扣到0并记欠费)
// -> 订单[充电中->已完成] 写 amount/paid/debt -> 扣款 -> 电桩[充电中->空闲]
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

    // 3. 读取用户余额, 计算"实扣/欠费"(BR-06):
    //    余额充足 -> 全额扣款, 欠费 0;
    //    余额不足 -> 把余额扣到 0, 差额 amount-balance 记为该订单的欠费(debt)。
    QSqlQuery balanceQuery(dbc);
    balanceQuery.prepare(QStringLiteral("SELECT balance FROM user WHERE user_id = :uid;"));
    balanceQuery.bindValue(QStringLiteral(":uid"), userId);
    if (!balanceQuery.exec() || !balanceQuery.next())
        return rollbackAndFail(QStringLiteral("查询用户余额失败"));
    const double userBalance = balanceQuery.value(0).toDouble();
    const double paid = qMin(amount, userBalance);          // 实际扣款(元)
    const double debt = round2(amount - paid);              // 欠费(元) = 应付 - 实扣
    qDebug() << "结算: 时长" << durationSecs / 60 << "分钟,"
             << "电量" << energy << "度, 应付" << amount << "元,"
             << "实扣" << paid << "元, 欠费" << debt << "元";

    // 4. 订单 充电中 -> 已完成, 写入电量/应付/实扣/欠费/结束时间
    QString stErr;
    if (!orderSetState(dbc, orderId, OrderCharging, OrderFinished, &stErr)) {
        rollbackTransaction();
        if (err) *err = stErr;
        return false;
    }
    QSqlQuery updateOrder(dbc);
    updateOrder.prepare(QStringLiteral(
        "UPDATE charging_order SET energy = :en, amount = :am, paid = :pd, debt = :db, end_time = :et"
        " WHERE order_id = :id AND status = 2;"));
    updateOrder.bindValue(QStringLiteral(":en"), energy);
    updateOrder.bindValue(QStringLiteral(":am"), amount);
    updateOrder.bindValue(QStringLiteral(":pd"), paid);
    updateOrder.bindValue(QStringLiteral(":db"), debt);
    updateOrder.bindValue(QStringLiteral(":et"), nowStr());
    updateOrder.bindValue(QStringLiteral(":id"), orderId);
    if (!updateOrder.exec()) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("写入订单结果失败: %1").arg(updateOrder.lastError().text()));
    }

    // 5. 扣用户余额(只扣"实扣"部分; paid <= 余额, 扣后余额 ≥ 0)
    QSqlQuery updateUser(dbc);
    updateUser.prepare(QStringLiteral(
        "UPDATE user SET balance = balance - :pd WHERE user_id = :uid AND balance >= :pd;"));
    updateUser.bindValue(QStringLiteral(":pd"), paid);
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
    qDebug() << "订单" << orderId << "结算成功, 实扣" << paid << "元, 欠费" << debt << "元";
    return true;
}

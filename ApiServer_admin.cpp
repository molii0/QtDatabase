#include "ApiServer.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTime>

#include <QtHttpServer/qhttpserverrequest.h>
#include <QtHttpServer/qhttpserverresponse.h>

// ============================================================================
// 管理端 REST 接口实现(登录/用户管理/电站桩管理/统计/订单)
// 管理端操作都要求请求头:  Authorization: Bearer <token>
// (token 由 POST /api/admin/login 下发, 保存在内存中)
// ============================================================================

namespace {
const QByteArray kAuthPrefix = QByteArrayLiteral("Bearer ");
}

// 取出请求头的 Authorization 值(去掉大小写差异)
static QByteArray authHeaderValue(const QHttpServerRequest &req)
{
    const QByteArrayView value = req.headers().value(QByteArrayLiteral("authorization"));
    return QByteArray(value.trimmed());
}

QHttpServerResponse ApiServer::unauthorized()
{
    return jsonError(401, QStringLiteral("未登录或 token 无效，请先登录"));
}

// 校验请求头里的 token; 通过返回 true 并回填账号
bool ApiServer::requireAdmin(const QHttpServerRequest &req, QString *account)
{
    const QByteArray auth = authHeaderValue(req);
    if (!auth.startsWith(kAuthPrefix))
        return false;
    const QString token = QString::fromLatin1(auth.mid(kAuthPrefix.size()));
    if (!m_tokens.contains(token))
        return false;
    if (account)
        *account = m_tokens.value(token);
    return true;
}

// POST /api/admin/login {account, password}
QHttpServerResponse ApiServer::onAdminLogin(const QHttpServerRequest &req)
{
    QJsonObject body;
    if (!parseBody(req, &body))
        return jsonError(400, QStringLiteral("请求体必须是 JSON"));
    const QString account = body.value(QStringLiteral("account")).toString().trimmed();
    const QString password = body.value(QStringLiteral("password")).toString();
    if (!m_db.checkAdminLogin(account, password))
        return jsonError(401, QStringLiteral("账号或密码错误"));

    // 生成一个随机 token 并记住(退出登录后失效)
    const QString token = newToken();
    m_tokens.insert(token, account);

    QJsonObject data;
    data[QStringLiteral("token")] = token;
    data[QStringLiteral("account")] = account;
    return jsonOk(data);
}

// POST /api/admin/logout
QHttpServerResponse ApiServer::onAdminLogout(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    const QByteArray auth = authHeaderValue(req);
    m_tokens.remove(QString::fromLatin1(auth.mid(kAuthPrefix.size())));

    QJsonObject data;
    data[QStringLiteral("message")] = QStringLiteral("已退出登录");
    return jsonOk(data);
}

// GET /api/admin/users?keyword=138
QHttpServerResponse ApiServer::onAdminListUsers(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    const QString keyword = req.query().queryItemValue(QStringLiteral("keyword"));
    QVector<DBManager::User> users;
    if (!m_db.listUsers(keyword, &users))
        return jsonError(500, QStringLiteral("查询用户失败"));

    QJsonArray arr;
    for (const DBManager::User &u : users)
        arr.append(userJson(u));
    QJsonObject data;
    data[QStringLiteral("users")] = arr;
    return jsonOk(data);
}

// PUT /api/admin/users/<id>/status {frozen: true|false}
QHttpServerResponse ApiServer::onAdminSetUserStatus(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    const qint64 userId = pathId(req.url().path(), 4);   // /api/admin/users/<id>/status
    QJsonObject body;
    if (userId <= 0 || !parseBody(req, &body)
        || !body.contains(QStringLiteral("frozen")))
        return jsonError(400, QStringLiteral("参数错误: 需要数字 userId 和 frozen(bool)"));
    const bool frozen = body.value(QStringLiteral("frozen")).toBool();

    QString err;
    if (!m_db.setUserStatus(userId, frozen, &err))
        return jsonError(dbErrorStatus(err), err);
    QJsonObject data;
    data[QStringLiteral("userId")] = static_cast<double>(userId);
    data[QStringLiteral("frozen")] = frozen;
    return jsonOk(data);
}

// GET /api/admin/stats  营收汇总 + 电桩状态分布
QHttpServerResponse ApiServer::onAdminStats(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    DBManager::RevenueSummary rev;
    QString err;
    if (!m_db.revenueSummary(&rev, &err))
        return jsonError(500, err);
    DBManager::ChargerStatusCount c;
    if (!m_db.chargerStatusCount(&c, &err))
        return jsonError(500, err);

    QJsonObject revenue;
    revenue[QStringLiteral("today")] = rev.today;
    revenue[QStringLiteral("month")] = rev.month;
    revenue[QStringLiteral("total")] = rev.total;

    QJsonObject charger;
    charger[QStringLiteral("idle")] = static_cast<double>(c.idle);
    charger[QStringLiteral("connected")] = static_cast<double>(c.connected);
    charger[QStringLiteral("charging")] = static_cast<double>(c.charging);
    charger[QStringLiteral("fault")] = static_cast<double>(c.fault);
    charger[QStringLiteral("total")] = static_cast<double>(c.total);

    QJsonObject data;
    data[QStringLiteral("revenue")] = revenue;
    data[QStringLiteral("charger")] = charger;
    return jsonOk(data);
}

// GET /api/admin/stats/daily?days=7
QHttpServerResponse ApiServer::onAdminDailyRevenue(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    bool okDays = false;
    const int days = req.query().queryItemValue(QStringLiteral("days")).toInt(&okDays);
    const int n = (okDays && days > 0 && days <= 366) ? days : 7;

    QVector<DBManager::RevenueDay> daily;
    QString err;
    if (!m_db.dailyRevenue(n, &daily, &err))
        return jsonError(500, err);

    QJsonArray arr;
    for (const DBManager::RevenueDay &d : daily) {
        QJsonObject item;
        item[QStringLiteral("day")] = d.day;
        item[QStringLiteral("amount")] = d.amount;
        item[QStringLiteral("orders")] = static_cast<double>(d.orders);
        arr.append(item);
    }
    QJsonObject data;
    data[QStringLiteral("daily")] = arr;
    return jsonOk(data);
}

// POST /api/admin/stations  新增电站
QHttpServerResponse ApiServer::onAdminCreateStation(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    QJsonObject body;
    if (!parseBody(req, &body))
        return jsonError(400, QStringLiteral("请求体必须是 JSON"));
    const QString name = body.value(QStringLiteral("name")).toString();
    const QString codePrefix = body.value(QStringLiteral("codePrefix")).toString();
    const QString address = body.value(QStringLiteral("address")).toString();
    const double longitude = body.value(QStringLiteral("longitude")).toDouble();
    const double latitude = body.value(QStringLiteral("latitude")).toDouble();
    const double price = body.value(QStringLiteral("price")).toDouble();
    // 可选: 分时电价峰/谷段价; 不给则按 平×1.35 / 平×0.55 自动推导
    const double pricePeak = body.contains(QStringLiteral("pricePeak"))
                                 ? body.value(QStringLiteral("pricePeak")).toDouble() : -1.0;
    const double priceValley = body.contains(QStringLiteral("priceValley"))
                                   ? body.value(QStringLiteral("priceValley")).toDouble() : -1.0;
    if (name.trimmed().isEmpty())
        return jsonError(400, QStringLiteral("充电站名称不能为空"));

    QString err;
    qint64 stationId = 0;
    if (!m_db.addStation(name, codePrefix, address, longitude, latitude, price,
                         &stationId, &err, pricePeak, priceValley))
        return jsonError(dbErrorStatus(err), err);

    QJsonObject data;
    data[QStringLiteral("stationId")] = static_cast<double>(stationId);
    data[QStringLiteral("message")] = QStringLiteral("新增充电站成功");
    return jsonCreated(data);
}

// PUT /api/admin/stations/<id>  修改电站
QHttpServerResponse ApiServer::onAdminUpdateStation(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    const qint64 stationId = pathId(req.url().path(), 4);
    QJsonObject body;
    if (stationId <= 0 || !parseBody(req, &body))
        return jsonError(400, QStringLiteral("参数错误"));
    const QString name = body.value(QStringLiteral("name")).toString();
    const QString codePrefix = body.value(QStringLiteral("codePrefix")).toString();
    const QString address = body.value(QStringLiteral("address")).toString();
    const double longitude = body.value(QStringLiteral("longitude")).toDouble();
    const double latitude = body.value(QStringLiteral("latitude")).toDouble();
    const double price = body.value(QStringLiteral("price")).toDouble();
    const double pricePeak = body.contains(QStringLiteral("pricePeak"))
                                 ? body.value(QStringLiteral("pricePeak")).toDouble() : -1.0;
    const double priceValley = body.contains(QStringLiteral("priceValley"))
                                   ? body.value(QStringLiteral("priceValley")).toDouble() : -1.0;
    if (name.trimmed().isEmpty())
        return jsonError(400, QStringLiteral("充电站名称不能为空"));

    QString err;
    if (!m_db.updateStation(stationId, name, codePrefix, address, longitude, latitude,
                            price, &err, pricePeak, priceValley))
        return jsonError(dbErrorStatus(err), err);

    QJsonObject data;
    data[QStringLiteral("message")] = QStringLiteral("修改充电站成功");
    return jsonOk(data);
}

// DELETE /api/admin/stations/<id>
QHttpServerResponse ApiServer::onAdminDeleteStation(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    const qint64 stationId = pathId(req.url().path(), 4);
    if (stationId <= 0)
        return jsonError(400, QStringLiteral("stationId 无效"));

    QString err;
    if (!m_db.deleteStation(stationId, &err))
        return jsonError(dbErrorStatus(err), err);

    QJsonObject data;
    data[QStringLiteral("message")] = QStringLiteral("删除充电站成功");
    return jsonOk(data);
}

// POST /api/admin/stations/<id>/chargers/batch {count, type, power}
QHttpServerResponse ApiServer::onAdminBatchChargers(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    const qint64 stationId = pathId(req.url().path(), 4);
    QJsonObject body;
    if (stationId <= 0 || !parseBody(req, &body))
        return jsonError(400, QStringLiteral("参数错误"));
    const int count = body.value(QStringLiteral("count")).toInt();
    const int type = body.value(QStringLiteral("type")).toInt();
    const double power = body.value(QStringLiteral("power")).toDouble();

    QString err;
    int created = 0;
    if (!m_db.batchAddChargers(stationId, count, type, power, &created, &err))
        return jsonError(dbErrorStatus(err), err);

    QJsonObject data;
    data[QStringLiteral("created")] = created;
    data[QStringLiteral("message")] = QStringLiteral("批量建桩成功");
    return jsonCreated(data);
}

// DELETE /api/admin/chargers/<id>
QHttpServerResponse ApiServer::onAdminDeleteCharger(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    const qint64 chargerId = pathId(req.url().path(), 4);
    if (chargerId <= 0)
        return jsonError(400, QStringLiteral("chargerId 无效"));

    QString err;
    if (!m_db.deleteCharger(chargerId, &err))
        return jsonError(dbErrorStatus(err), err);

    QJsonObject data;
    data[QStringLiteral("message")] = QStringLiteral("删除电桩成功");
    return jsonOk(data);
}

// PUT /api/admin/chargers/<id>/action {action: "fault" | "recover" | "restart"}
QHttpServerResponse ApiServer::onAdminChargerAction(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    const qint64 chargerId = pathId(req.url().path(), 4);
    QJsonObject body;
    if (chargerId <= 0 || !parseBody(req, &body))
        return jsonError(400, QStringLiteral("参数错误"));
    const QString action = body.value(QStringLiteral("action")).toString();
    if (action != QStringLiteral("fault") && action != QStringLiteral("recover")
        && action != QStringLiteral("restart"))
        return jsonError(400, QStringLiteral("action 只能是 fault / recover / restart"));

    QString err;
    bool ok = false;
    if (action == QStringLiteral("fault"))
        ok = m_db.adminMarkFault(chargerId, &err);
    else   // recover / restart 都是"故障 -> 空闲"
        ok = m_db.adminRecover(chargerId, &err);
    if (!ok)
        return jsonError(dbErrorStatus(err), err);

    DBManager::Charger c;
    m_db.getCharger(chargerId, &c);

    // 记录一条运维日志(UC-A-05)
    const QString actionText = action == QStringLiteral("fault") ? QStringLiteral("标记故障")
                               : action == QStringLiteral("restart") ? QStringLiteral("远程重启")
                                                                     : QStringLiteral("恢复正常");
    m_db.addOpsLog(account, chargerId, c.code, actionText,
                   QStringLiteral("由管理端 %1 执行").arg(account));

    QJsonObject data;
    data[QStringLiteral("chargerId")] = static_cast<double>(chargerId);
    data[QStringLiteral("status")] = c.status;
    data[QStringLiteral("statusText")] = chargerStateText(c.status);
    return jsonOk(data);
}

// GET /api/admin/orders?userId=1&status=2
QHttpServerResponse ApiServer::onAdminListOrders(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    const qint64 userId = parseId(req.query().queryItemValue(QStringLiteral("userId")));
    bool okSt = false;
    const int status = req.query().queryItemValue(QStringLiteral("status")).toInt(&okSt);

    QVector<DBManager::Order> orders = m_db.listOrders(userId > 0 ? userId : -1);
    QJsonArray arr;
    for (const DBManager::Order &o : orders) {
        if (okSt && o.status != status)
            continue;   // 客户端再按状态筛一道
        arr.append(orderJson(o));
    }
    QJsonObject data;
    data[QStringLiteral("orders")] = arr;
    return jsonOk(data);
}

// GET /api/admin/logs?limit=50  运维日志(倒序, 最近 limit 条)
QHttpServerResponse ApiServer::onAdminOpsLogs(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    bool okLimit = false;
    const int rawLimit = req.query().queryItemValue(QStringLiteral("limit")).toInt(&okLimit);
    const int limit = (okLimit && rawLimit > 0) ? rawLimit : 50;

    QVector<DBManager::OpsLog> logs;
    QString err;
    if (!m_db.listOpsLogs(limit, &logs, &err))
        return jsonError(500, err);

    QJsonArray arr;
    for (const DBManager::OpsLog &o : logs) {
        QJsonObject item;
        item[QStringLiteral("logId")] = static_cast<double>(o.logId);
        item[QStringLiteral("adminAccount")] = o.adminAccount;
        item[QStringLiteral("chargerId")] = static_cast<double>(o.chargerId);
        item[QStringLiteral("chargerCode")] = o.chargerCode;
        item[QStringLiteral("action")] = o.action;
        item[QStringLiteral("detail")] = o.detail;
        item[QStringLiteral("createdAt")] = o.createdAt;
        arr.append(item);
    }
    QJsonObject data;
    data[QStringLiteral("logs")] = arr;
    return jsonOk(data);
}

// GET /api/admin/stats/by-station?days=30  近 days 天各电站营收排行(降序)
QHttpServerResponse ApiServer::onAdminRevenueByStation(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    bool okDays = false;
    const int days = req.query().queryItemValue(QStringLiteral("days")).toInt(&okDays);
    const int n = (okDays && days > 0 && days <= 366) ? days : 30;

    QVector<DBManager::RevenueByItem> items;
    QString err;
    if (!m_db.revenueByStation(n, &items, &err))
        return jsonError(500, err);

    QJsonArray arr;
    for (const DBManager::RevenueByItem &r : items) {
        QJsonObject item;
        item[QStringLiteral("id")] = static_cast<double>(r.id);
        item[QStringLiteral("name")] = r.name;
        item[QStringLiteral("amount")] = r.amount;
        item[QStringLiteral("orders")] = static_cast<double>(r.orders);
        arr.append(item);
    }
    QJsonObject data;
    data[QStringLiteral("items")] = arr;
    return jsonOk(data);
}

// GET /api/admin/stats/by-charger?days=30  近 days 天各电桩营收排行(降序)
QHttpServerResponse ApiServer::onAdminRevenueByCharger(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    bool okDays = false;
    const int days = req.query().queryItemValue(QStringLiteral("days")).toInt(&okDays);
    const int n = (okDays && days > 0 && days <= 366) ? days : 30;

    QVector<DBManager::RevenueByItem> items;
    QString err;
    if (!m_db.revenueByCharger(n, &items, &err))
        return jsonError(500, err);

    QJsonArray arr;
    for (const DBManager::RevenueByItem &r : items) {
        QJsonObject item;
        item[QStringLiteral("id")] = static_cast<double>(r.id);
        item[QStringLiteral("name")] = r.name;
        item[QStringLiteral("amount")] = r.amount;
        item[QStringLiteral("orders")] = static_cast<double>(r.orders);
        arr.append(item);
    }
    QJsonObject data;
    data[QStringLiteral("items")] = arr;
    return jsonOk(data);
}

// POST /api/admin/demo/history {days?, density?} —— 演示专用
// 给 Web/图表演示批量造"历史数据"(向前补足最近 days 天的历史订单, 并补齐
// 充值流水/运维日志/负荷预测)。仅作开发/演示工具, 不属于正式业务接口。
QHttpServerResponse ApiServer::onAdminDemoGen(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    QJsonObject body;
    if (!parseBody(req, &body))
        return jsonError(400, QStringLiteral("请求体必须是 JSON"));

    int days = body.value(QStringLiteral("days")).toInt();
    if (days == 0)
        days = 30;                                  // 默认 30 天
    if (days < 1 || days > 730)
        return jsonError(400, QStringLiteral("days 需要 1~730"));
    double density = body.value(QStringLiteral("density")).toDouble();
    if (density <= 0.0)
        density = 1.0;
    if (density < 0.1 || density > 20.0)
        return jsonError(400, QStringLiteral("density 需要 0.1~20"));

    DBManager::DemoGenResult r;
    QString err;
    if (!m_db.generateDemoHistory(days, density, &r, &err))
        return jsonError(dbErrorStatus(err), err);

    QJsonObject data;
    data[QStringLiteral("demoOnly")] = true;        // 开发/演示专用标记
    data[QStringLiteral("message")] =
        r.daysGenerated == 0
            ? QStringLiteral("历史已覆盖, 无需生成(演示数据)")
            : QStringLiteral("演示历史数据已生成");
    data[QStringLiteral("daysRequested")] = r.daysRequested;
    data[QStringLiteral("daysGenerated")] = r.daysGenerated;
    data[QStringLiteral("ordersAdded")] = static_cast<double>(r.ordersAdded);
    data[QStringLiteral("rechargesAdded")] = static_cast<double>(r.rechargesAdded);
    data[QStringLiteral("opsLogsAdded")] = static_cast<double>(r.opsLogsAdded);
    data[QStringLiteral("predictionsAdded")] = static_cast<double>(r.predictionsAdded);
    return jsonOk(data);
}

// GET /api/admin/predictions?hours=24&stationId=  —— 管理端"智能预测"页
// 返回: 未来 hours 小时的逐时预测曲线 + 各站预测电量(数据源 load_prediction)
QHttpServerResponse ApiServer::onAdminPredictions(const QHttpServerRequest &req)
{
    QString account;
    if (!requireAdmin(req, &account))
        return unauthorized();

    bool okHours = false;
    int hours = req.query().queryItemValue(QStringLiteral("hours")).toInt(&okHours);
    if (!okHours || hours <= 0 || hours > 168)
        hours = 24;                                     // 默认未来 24 小时
    // stationId 可选: 只预测一个站; 不传/非法 = 全部站
    const qint64 stationId = parseId(req.query().queryItemValue(QStringLiteral("stationId")));

    QString generatedAt, err;
    QVector<DBManager::PredictionPoint> curve;
    QVector<DBManager::PredictionByStation> byStation;
    if (!m_db.listPredictions(hours, stationId, &curve, &byStation, &generatedAt, &err))
        return jsonError(500, err);

    // 逐时曲线(前端直接画折线; isPeak 用于给高峰时段标色/底色)
    QJsonArray curveArr;
    for (const DBManager::PredictionPoint &p : curve) {
        QJsonObject item;
        item[QStringLiteral("targetTime")] = p.targetTime;
        item[QStringLiteral("loadKwh")] = p.loadKwh;
        item[QStringLiteral("idleCount")] = static_cast<double>(p.idleCount);
        item[QStringLiteral("isPeak")] = (p.isPeak == 1);
        item[QStringLiteral("stations")] = static_cast<double>(p.stations);
        curveArr.append(item);
    }

    // 各站预测电量(前端画柱状图/排行榜)
    QJsonArray stationArr;
    for (const DBManager::PredictionByStation &s : byStation) {
        QJsonObject item;
        item[QStringLiteral("stationId")] = static_cast<double>(s.stationId);
        item[QStringLiteral("name")] = s.name;
        item[QStringLiteral("loadKwh")] = s.loadKwh;
        item[QStringLiteral("points")] = static_cast<double>(s.points);
        item[QStringLiteral("avgIdle")] = s.avgIdle;
        stationArr.append(item);
    }

    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime from(QDate(now.date()), QTime(now.time().hour(), 0));   // 当前整点
    const QDateTime to = from.addSecs(static_cast<qint64>(hours) * 3600);

    QJsonObject data;
    data[QStringLiteral("hours")] = hours;
    data[QStringLiteral("from")] = from.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    data[QStringLiteral("to")] = to.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    data[QStringLiteral("generatedAt")] = generatedAt.isEmpty()
                                              ? QJsonValue(QJsonValue::Null)
                                              : QJsonValue(generatedAt);
    if (stationId > 0)
        data[QStringLiteral("stationId")] = static_cast<double>(stationId);
    data[QStringLiteral("curve")] = curveArr;
    data[QStringLiteral("stations")] = stationArr;
    if (curve.isEmpty()) {
        data[QStringLiteral("message")] = QStringLiteral(
            "暂无未来预测数据(演示库可跑 QtDatabase.exe --gen-history 或调 POST /api/admin/demo/history 补充)");
    }
    return jsonOk(data);
}

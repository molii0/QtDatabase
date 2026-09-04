#include "ApiServer.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>

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
    const QString token = QString::number(QRandomGenerator::global()->generate64(), 16)
                        + QString::number(QRandomGenerator::global()->generate64(), 16);
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
    if (name.trimmed().isEmpty())
        return jsonError(400, QStringLiteral("充电站名称不能为空"));

    QString err;
    qint64 stationId = 0;
    if (!m_db.addStation(name, codePrefix, address, longitude, latitude, price,
                         &stationId, &err))
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
    if (name.trimmed().isEmpty())
        return jsonError(400, QStringLiteral("充电站名称不能为空"));

    QString err;
    if (!m_db.updateStation(stationId, name, codePrefix, address, longitude, latitude, price, &err))
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

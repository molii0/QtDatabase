#include "ApiServer.h"

#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPair>
#include <QRandomGenerator>
#include <QTcpServer>

#include <algorithm>
#include <cmath>

#include <QtHttpServer/qhttpserver.h>
#include <QtHttpServer/qhttpserverrequest.h>
#include <QtHttpServer/qhttpserverresponse.h>

// ============================================================================
// ApiServer 实现: 生命周期 / 路由注册 / 公共工具 / C端接口
// (管理端接口在 ApiServer_admin.cpp)
// ============================================================================

namespace {

// 球面距离(Haversine 公式), 返回公里
double haversineKm(double lat1, double lon1, double lat2, double lon2)
{
    const double kR = 6371.0;   // 地球半径(km)
    auto rad = [](double d) { return d * M_PI / 180.0; };
    const double dLat = rad(lat2 - lat1);
    const double dLon = rad(lon2 - lon1);
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2)
                   + std::cos(rad(lat1)) * std::cos(rad(lat2))
                     * std::sin(dLon / 2) * std::sin(dLon / 2);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return kR * c;
}

} // namespace

ApiServer::ApiServer()
    : m_db(DBManager::instance())
{
}

ApiServer::~ApiServer()
{
    delete m_httpServer;    // 同时释放它下面的 QTcpServer 子对象
}

bool ApiServer::start(quint16 port)
{
    // QHttpServer 的用法: 先建一个 QTcpServer 监听端口, 再 bind 给它
    m_httpServer = new QHttpServer;
    QTcpServer *tcp = new QTcpServer(m_httpServer);
    if (!tcp->listen(QHostAddress::Any, port)) {
        qCritical() << "服务器启动失败, 端口" << port << "可能被占用";
        return false;
    }

    // ---------------- C 端 ----------------
    m_httpServer->route("/api/users/login", QHttpServerRequest::Method::Post,
                  [this](const QHttpServerRequest &req) { return onUserLogin(req); });
    m_httpServer->route("/api/stations", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onListStations(req); });
    m_httpServer->route("/api/stations/nearby", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onNearbyStations(req); });
    m_httpServer->route("/api/chargers", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onListChargers(req); });
    m_httpServer->route("/api/charges", QHttpServerRequest::Method::Post,
                  [this](const QHttpServerRequest &req) { return onCreateCharge(req); });
    m_httpServer->route("/api/charges/<arg>/start", QHttpServerRequest::Method::Post,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onStartCharge(req);
                  });
    m_httpServer->route("/api/charges/<arg>/finish", QHttpServerRequest::Method::Post,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onFinishCharge(req);
                  });
    m_httpServer->route("/api/charges/<arg>", QHttpServerRequest::Method::Delete,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onCancelCharge(req);
                  });
    m_httpServer->route("/api/users/<arg>/orders", QHttpServerRequest::Method::Get,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onUserOrders(req);
                  });
    m_httpServer->route("/api/users/<arg>/recharge", QHttpServerRequest::Method::Post,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onRecharge(req);
                  });
    m_httpServer->route("/api/users/<arg>/profile", QHttpServerRequest::Method::Put,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onUpdateProfile(req);
                  });
    m_httpServer->route("/api/users/<arg>/active-order", QHttpServerRequest::Method::Get,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onActiveOrder(req);
                  });

    // ---------------- 管理端 ----------------
    m_httpServer->route("/api/admin/login", QHttpServerRequest::Method::Post,
                  [this](const QHttpServerRequest &req) { return onAdminLogin(req); });
    m_httpServer->route("/api/admin/logout", QHttpServerRequest::Method::Post,
                  [this](const QHttpServerRequest &req) { return onAdminLogout(req); });
    m_httpServer->route("/api/admin/users", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onAdminListUsers(req); });
    m_httpServer->route("/api/admin/users/<arg>/status", QHttpServerRequest::Method::Put,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onAdminSetUserStatus(req);
                  });
    m_httpServer->route("/api/admin/stats", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onAdminStats(req); });
    m_httpServer->route("/api/admin/stats/daily", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onAdminDailyRevenue(req); });
    m_httpServer->route("/api/admin/stats/by-station", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onAdminRevenueByStation(req); });
    m_httpServer->route("/api/admin/stats/by-charger", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onAdminRevenueByCharger(req); });
    m_httpServer->route("/api/admin/stations", QHttpServerRequest::Method::Post,
                  [this](const QHttpServerRequest &req) { return onAdminCreateStation(req); });
    m_httpServer->route("/api/admin/stations/<arg>", QHttpServerRequest::Method::Put,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onAdminUpdateStation(req);
                  });
    m_httpServer->route("/api/admin/stations/<arg>", QHttpServerRequest::Method::Delete,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onAdminDeleteStation(req);
                  });
    m_httpServer->route("/api/admin/stations/<arg>/chargers/batch", QHttpServerRequest::Method::Post,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onAdminBatchChargers(req);
                  });
    m_httpServer->route("/api/admin/chargers/<arg>", QHttpServerRequest::Method::Delete,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onAdminDeleteCharger(req);
                  });
    m_httpServer->route("/api/admin/chargers/<arg>/action", QHttpServerRequest::Method::Put,
                  [this](const QString &id, const QHttpServerRequest &req) {
                      Q_UNUSED(id);
                      return onAdminChargerAction(req);
                  });
    m_httpServer->route("/api/admin/orders", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onAdminListOrders(req); });
    m_httpServer->route("/api/admin/logs", QHttpServerRequest::Method::Get,
                  [this](const QHttpServerRequest &req) { return onAdminOpsLogs(req); });

    // 注册成功后再真正开始监听
    if (!m_httpServer->bind(tcp)) {
        qCritical() << "绑定 QHttpServer 失败";
        return false;
    }
    qDebug() << "REST 服务器启动成功, 端口" << port;
    return true;
}

// ------------------------- 公共工具 -------------------------

QHttpServerResponse ApiServer::dbResult(bool ok, const QString &err,
                                        const QJsonObject &okData, int okStatus)
{
    if (ok)
        return okStatus == 201 ? jsonCreated(okData) : jsonOk(okData);
    return jsonError(dbErrorStatus(err), err);
}

int ApiServer::dbErrorStatus(const QString &msg)
{
    if (msg.contains(QStringLiteral("不存在")))
        return 404;
    if (msg.contains(QStringLiteral("冻结")))
        return 403;
    if (msg.isEmpty())
        return 500;
    return 409;      // 状态冲突/占用/余额不足等业务冲突
}

QHttpServerResponse ApiServer::jsonError(int status, const QString &msg)
{
    QJsonObject body;
    body[QStringLiteral("error")] = msg;
    return QHttpServerResponse(body, static_cast<QHttpServerResponder::StatusCode>(status));
}

QHttpServerResponse ApiServer::jsonOk(const QJsonObject &body)
{
    return QHttpServerResponse(body, QHttpServerResponder::StatusCode::Ok);
}

QHttpServerResponse ApiServer::jsonCreated(const QJsonObject &body)
{
    return QHttpServerResponse(body, QHttpServerResponder::StatusCode::Created);
}

QHttpServerResponse ApiServer::forbidden(const QString &msg)
{
    return jsonError(403, msg);
}

QString ApiServer::newToken()
{
    return QString::number(QRandomGenerator::global()->generate64(), 16)
         + QString::number(QRandomGenerator::global()->generate64(), 16);
}

bool ApiServer::parseBody(const QHttpServerRequest &req, QJsonObject *out)
{
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(req.body(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    *out = doc.object();
    return true;
}

qint64 ApiServer::parseId(const QString &text)
{
    bool ok = false;
    const qint64 id = text.toLongLong(&ok);
    return ok ? id : -1;
}

qint64 ApiServer::pathId(const QString &path, int index)
{
    return parseId(path.split(QLatin1Char('/')).value(index));
}

// 从请求头取 Bearer token(为空表示未带)
static QString bearerToken(const QHttpServerRequest &req)
{
    const QByteArray value = QByteArray(req.headers().value(QByteArrayLiteral("authorization")).trimmed());
    if (!value.startsWith("Bearer "))
        return QString();
    return QString::fromLatin1(value.mid(7));
}

// 用户端鉴权: token 有效则回填 userId
bool ApiServer::requireUser(const QHttpServerRequest &req, qint64 *userId)
{
    const QString token = bearerToken(req);
    if (token.isEmpty() || !m_userTokens.contains(token))
        return false;
    if (userId)
        *userId = m_userTokens.value(token);
    return true;
}

// ------------------------- JSON 序列化 -------------------------

QJsonObject ApiServer::userJson(const DBManager::User &u) const
{
    QJsonObject o;
    o[QStringLiteral("userId")] = static_cast<double>(u.userId);
    o[QStringLiteral("phone")] = u.phone;
    o[QStringLiteral("nickname")] = u.nickname;
    o[QStringLiteral("avatar")] = u.avatar;
    o[QStringLiteral("balance")] = u.balance;
    o[QStringLiteral("status")] = u.status;         // 1=正常 0=冻结
    o[QStringLiteral("statusText")] = u.status == 1 ? QStringLiteral("正常") : QStringLiteral("冻结");
    o[QStringLiteral("registerTime")] = u.registerTime;
    return o;
}

QJsonObject ApiServer::stationJson(const DBManager::Station &s, double distanceKm) const
{
    QJsonObject o;
    o[QStringLiteral("stationId")] = static_cast<double>(s.stationId);
    o[QStringLiteral("name")] = s.name;
    o[QStringLiteral("address")] = s.address;
    o[QStringLiteral("longitude")] = s.longitude;
    o[QStringLiteral("latitude")] = s.latitude;
    o[QStringLiteral("price")] = s.price;
    o[QStringLiteral("codePrefix")] = s.codePrefix;
    // 统计该站电桩状态(空闲/已连接/充电中/故障)
    int total = 0, idle = 0, connected = 0, charging = 0, fault = 0;
    const QVector<DBManager::Charger> chargers = m_db.listChargers(s.stationId);
    for (const DBManager::Charger &ch : chargers) {
        ++total;
        if (ch.status == ChargerIdle)           ++idle;
        else if (ch.status == ChargerConnected) ++connected;
        else if (ch.status == ChargerCharging)  ++charging;
        else if (ch.status == ChargerFault)     ++fault;
    }
    o[QStringLiteral("totalChargers")] = total;
    o[QStringLiteral("idleChargers")] = idle;
    o[QStringLiteral("connectedChargers")] = connected;
    o[QStringLiteral("chargingChargers")] = charging;
    o[QStringLiteral("faultChargers")] = fault;
    // 在线率 = 非故障桩占比
    o[QStringLiteral("onlineRate")] =
        total > 0 ? std::round((total - fault) * 100.0 / total) : 100.0;
    if (distanceKm >= 0.0)
        o[QStringLiteral("distanceKm")] = std::round(distanceKm * 10.0) / 10.0;  // 保留1位
    return o;
}

QJsonObject ApiServer::chargerJson(const DBManager::Charger &c) const
{
    QJsonObject o;
    o[QStringLiteral("chargerId")] = static_cast<double>(c.chargerId);
    o[QStringLiteral("stationId")] = static_cast<double>(c.stationId);
    o[QStringLiteral("code")] = c.code;
    o[QStringLiteral("type")] = c.type;
    o[QStringLiteral("typeText")] = c.type == 1 ? QStringLiteral("快充") : QStringLiteral("慢充");
    o[QStringLiteral("power")] = c.power;
    o[QStringLiteral("status")] = c.status;
    o[QStringLiteral("statusText")] = chargerStateText(c.status);
    return o;
}

QJsonObject ApiServer::orderJson(const DBManager::Order &o) const
{
    QJsonObject j;
    j[QStringLiteral("orderId")] = static_cast<double>(o.orderId);
    j[QStringLiteral("orderNo")] = o.orderNo;
    j[QStringLiteral("userId")] = static_cast<double>(o.userId);
    j[QStringLiteral("userPhone")] = o.userPhone;
    j[QStringLiteral("stationId")] = static_cast<double>(o.stationId);
    j[QStringLiteral("stationName")] = o.stationName;
    j[QStringLiteral("chargerId")] = static_cast<double>(o.chargerId);
    j[QStringLiteral("chargerCode")] = o.chargerCode;
    j[QStringLiteral("status")] = o.status;
    j[QStringLiteral("statusText")] = orderStateText(o.status);
    j[QStringLiteral("energy")] = o.energy;
    j[QStringLiteral("amount")] = o.amount;
    j[QStringLiteral("paid")] = o.paid;
    j[QStringLiteral("debt")] = o.debt;
    j[QStringLiteral("startTime")] = o.startTime;
    j[QStringLiteral("endTime")] = o.endTime;
    return j;
}

// ------------------------- C 端接口 -------------------------

// POST /api/users/login  手机号免密登录(不存在自动注册); 返回用户 + token
QHttpServerResponse ApiServer::onUserLogin(const QHttpServerRequest &req)
{
    QJsonObject body;
    if (!parseBody(req, &body))
        return jsonError(400, QStringLiteral("请求体必须是 JSON"));
    QString phone = body.value(QStringLiteral("phone")).toString().trimmed();
    if (phone.size() != 11 || !phone.startsWith(QLatin1Char('1')))
        return jsonError(400, QStringLiteral("请输入正确的 11 位手机号"));

    DBManager::User u;
    if (!m_db.getUserByPhone(phone, &u)) {
        QString nickname = body.value(QStringLiteral("nickname")).toString().trimmed();
        if (nickname.isEmpty())
            nickname = QStringLiteral("用户%1").arg(phone.right(4));
        if (!m_db.insertUser(phone, nickname))
            return jsonError(500, QStringLiteral("自动注册失败"));
        m_db.getUserByPhone(phone, &u);
    }
    if (u.status == 0)
        return jsonError(403, QStringLiteral("账号已被冻结，请联系客服"));

    const QString token = newToken();
    m_userTokens.insert(token, u.userId);

    QJsonObject data;
    data[QStringLiteral("token")] = token;
    data[QStringLiteral("user")] = userJson(u);
    return jsonOk(data);
}

// GET /api/stations
QHttpServerResponse ApiServer::onListStations(const QHttpServerRequest &req)
{
    Q_UNUSED(req);
    QJsonArray arr;
    const QVector<DBManager::Station> stations = m_db.listStations();
    for (const DBManager::Station &s : stations)
        arr.append(stationJson(s));
    QJsonObject data;
    data[QStringLiteral("stations")] = arr;
    return jsonOk(data);
}

// GET /api/stations/nearby?latitude=..&longitude=..  按距离升序
QHttpServerResponse ApiServer::onNearbyStations(const QHttpServerRequest &req)
{
    bool okLat = false, okLng = false;
    const double lat = req.query().queryItemValue(QStringLiteral("latitude")).toDouble(&okLat);
    const double lng = req.query().queryItemValue(QStringLiteral("longitude")).toDouble(&okLng);
    if (!okLat || !okLng || lat < -90 || lat > 90 || lng < -180 || lng > 180)
        return jsonError(400, QStringLiteral("请提供合法的 latitude/longitude"));

    QVector<QPair<double, DBManager::Station>> withDist;   // 距离 -> 电站
    const QVector<DBManager::Station> stations = m_db.listStations();
    for (const DBManager::Station &s : stations) {
        const double d = haversineKm(lat, lng, s.latitude, s.longitude);
        withDist.append({d, s});
    }
    std::sort(withDist.begin(), withDist.end(),
              [](const QPair<double, DBManager::Station> &a,
                 const QPair<double, DBManager::Station> &b) { return a.first < b.first; });

    QJsonArray arr;
    for (const auto &item : withDist)
        arr.append(stationJson(item.second, item.first));
    QJsonObject data;
    data[QStringLiteral("stations")] = arr;
    return jsonOk(data);
}

// GET /api/chargers?stationId=1   (不带参数返回全部)
QHttpServerResponse ApiServer::onListChargers(const QHttpServerRequest &req)
{
    const qint64 stationId = parseId(req.query().queryItemValue(QStringLiteral("stationId")));
    QJsonArray arr;
    const QVector<DBManager::Charger> chargers = m_db.listChargers(stationId);
    for (const DBManager::Charger &c : chargers)
        arr.append(chargerJson(c));
    QJsonObject data;
    data[QStringLiteral("chargers")] = arr;
    return jsonOk(data);
}

// POST /api/charges {chargerId}  选桩下单(带用户 token; 待支付, 电桩已连接)
QHttpServerResponse ApiServer::onCreateCharge(const QHttpServerRequest &req)
{
    qint64 authId = 0;
    if (!requireUser(req, &authId))
        return unauthorized();

    QJsonObject body;
    if (!parseBody(req, &body))
        return jsonError(400, QStringLiteral("请求体必须是 JSON"));
    // 可选传 userId, 但必须等于 token 对应的用户
    const qint64 bodyUserId = static_cast<qint64>(body.value(QStringLiteral("userId")).toDouble());
    if (bodyUserId > 0 && bodyUserId != authId)
        return forbidden(QStringLiteral("无权操作其他用户账号"));
    const qint64 chargerId = static_cast<qint64>(body.value(QStringLiteral("chargerId")).toDouble());
    if (chargerId <= 0)
        return jsonError(400, QStringLiteral("缺少 chargerId"));

    QString err;
    qint64 orderId = 0;
    if (!m_db.orderConnect(authId, chargerId, &orderId, &err))
        return jsonError(dbErrorStatus(err), err);

    DBManager::Order o;
    m_db.getOrderById(orderId, &o);
    QJsonObject data;
    data[QStringLiteral("order")] = orderJson(o);
    return jsonCreated(data);
}

QHttpServerResponse ApiServer::onStartCharge(const QHttpServerRequest &req)
{
    qint64 authId = 0;
    if (!requireUser(req, &authId))
        return unauthorized();
    const qint64 orderId = pathId(req.url().path(), 3);   // /api/charges/<id>/start
    if (orderId <= 0)
        return jsonError(400, QStringLiteral("订单号无效"));

    DBManager::Order o;
    if (!m_db.getOrderById(orderId, &o))
        return jsonError(404, QStringLiteral("订单不存在"));
    if (o.userId != authId)
        return forbidden(QStringLiteral("无权操作他人的订单"));

    QString err;
    if (!m_db.orderStart(orderId, &err))
        return jsonError(dbErrorStatus(err), err);
    m_db.getOrderById(orderId, &o);
    return jsonOk(orderJson(o));
}

QHttpServerResponse ApiServer::onFinishCharge(const QHttpServerRequest &req)
{
    qint64 authId = 0;
    if (!requireUser(req, &authId))
        return unauthorized();
    const qint64 orderId = pathId(req.url().path(), 3);
    if (orderId <= 0)
        return jsonError(400, QStringLiteral("订单号无效"));

    DBManager::Order o;
    if (!m_db.getOrderById(orderId, &o))
        return jsonError(404, QStringLiteral("订单不存在"));
    if (o.userId != authId)
        return forbidden(QStringLiteral("无权操作他人的订单"));

    QString err;
    if (!m_db.orderFinish(orderId, &err))
        return jsonError(dbErrorStatus(err), err);
    m_db.getOrderById(orderId, &o);
    return jsonOk(orderJson(o));
}

QHttpServerResponse ApiServer::onCancelCharge(const QHttpServerRequest &req)
{
    qint64 authId = 0;
    if (!requireUser(req, &authId))
        return unauthorized();
    const qint64 orderId = pathId(req.url().path(), 3);   // /api/charges/<id>
    if (orderId <= 0)
        return jsonError(400, QStringLiteral("订单号无效"));

    DBManager::Order o;
    if (!m_db.getOrderById(orderId, &o))
        return jsonError(404, QStringLiteral("订单不存在"));
    if (o.userId != authId)
        return forbidden(QStringLiteral("无权操作他人的订单"));

    QString err;
    if (!m_db.orderCancel(orderId, &err))
        return jsonError(dbErrorStatus(err), err);
    QJsonObject data;
    data[QStringLiteral("message")] = QStringLiteral("订单已取消");
    return jsonOk(data);
}

// GET /api/users/<id>/orders(需该用户的 token)
QHttpServerResponse ApiServer::onUserOrders(const QHttpServerRequest &req)
{
    qint64 authId = 0;
    if (!requireUser(req, &authId))
        return unauthorized();
    const qint64 userId = pathId(req.url().path(), 3);
    if (userId <= 0 || userId != authId)
        return forbidden(QStringLiteral("无权查看其他用户的数据"));

    QJsonArray arr;
    const QVector<DBManager::Order> orders = m_db.listOrders(userId);
    for (const DBManager::Order &o : orders)
        arr.append(orderJson(o));
    QJsonObject data;
    data[QStringLiteral("orders")] = arr;
    return jsonOk(data);
}

// POST /api/users/<id>/recharge {amount}(需该用户的 token)
QHttpServerResponse ApiServer::onRecharge(const QHttpServerRequest &req)
{
    qint64 authId = 0;
    if (!requireUser(req, &authId))
        return unauthorized();
    const qint64 userId = pathId(req.url().path(), 3);
    if (userId <= 0 || userId != authId)
        return forbidden(QStringLiteral("无权操作其他用户的账号"));

    QJsonObject body;
    if (!parseBody(req, &body))
        return jsonError(400, QStringLiteral("请求体必须是 JSON"));
    const double amount = body.value(QStringLiteral("amount")).toDouble();
    if (amount <= 0)
        return jsonError(400, QStringLiteral("充值金额必须大于 0"));

    DBManager::User u;
    if (!m_db.getUserById(userId, &u))
        return jsonError(404, QStringLiteral("用户不存在"));
    if (!m_db.updateBalance(userId, amount))
        return jsonError(500, QStringLiteral("充值失败"));
    m_db.getUserById(userId, &u);
    QJsonObject data;
    data[QStringLiteral("user")] = userJson(u);
    return jsonOk(data);
}

// PUT /api/users/<id>/profile {nickname?, avatar?}(需该用户的 token)
QHttpServerResponse ApiServer::onUpdateProfile(const QHttpServerRequest &req)
{
    qint64 authId = 0;
    if (!requireUser(req, &authId))
        return unauthorized();
    const qint64 userId = pathId(req.url().path(), 3);
    if (userId <= 0 || userId != authId)
        return forbidden(QStringLiteral("无权修改其他用户的资料"));

    QJsonObject body;
    if (!parseBody(req, &body))
        return jsonError(400, QStringLiteral("请求体必须是 JSON"));
    const bool hasNickname = body.contains(QStringLiteral("nickname"));
    const bool hasAvatar = body.contains(QStringLiteral("avatar"));
    if (!hasNickname && !hasAvatar)
        return jsonError(400, QStringLiteral("没有需要修改的字段(至少给 nickname 或 avatar)"));

    QString err;
    if (hasNickname && !m_db.updateNickname(userId, body.value(QStringLiteral("nickname")).toString(), &err))
        return jsonError(dbErrorStatus(err), err);
    if (hasAvatar && !m_db.updateAvatar(userId, body.value(QStringLiteral("avatar")).toString(), &err))
        return jsonError(dbErrorStatus(err), err);

    DBManager::User u;
    m_db.getUserById(userId, &u);
    QJsonObject data;
    data[QStringLiteral("user")] = userJson(u);
    return jsonOk(data);
}

// GET /api/users/<id>/active-order  当前未结算订单(需该用户的 token)
QHttpServerResponse ApiServer::onActiveOrder(const QHttpServerRequest &req)
{
    qint64 authId = 0;
    if (!requireUser(req, &authId))
        return unauthorized();
    const qint64 userId = pathId(req.url().path(), 3);
    if (userId <= 0 || userId != authId)
        return forbidden(QStringLiteral("无权查看其他用户的数据"));

    DBManager::Order o;
    QString err;
    const bool found = m_db.getActiveOrderOfUser(userId, &o, &err);
    if (!err.isEmpty())
        return jsonError(500, err);
    QJsonObject data;
    if (found)
        data[QStringLiteral("order")] = orderJson(o);
    else
        data[QStringLiteral("order")] = QJsonValue(QJsonValue::Null);   // 没有未结算订单
    return jsonOk(data);
}

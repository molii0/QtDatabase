#include "ApiServer.h"

#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QTcpServer>

#include <QtHttpServer/qhttpserver.h>
#include <QtHttpServer/qhttpserverrequest.h>
#include <QtHttpServer/qhttpserverresponse.h>

// ============================================================================
// ApiServer 实现: 生命周期 / 路由注册 / 公共工具 / C端接口
// (管理端接口在 ApiServer_admin.cpp)
// ============================================================================

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

    // 注册成功后再真正开始监听
    if (!m_httpServer->bind(tcp)) {
        qCritical() << "绑定 QHttpServer 失败";
        return false;
    }
    qDebug() << "REST 服务器启动成功, 端口" << port;
    return true;
}

// 把 DB 操作结果包成标准 HTTP 响应: 成功 okStatus + okData, 失败按文案选状态码
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

// ---------------- JSON 序列化 ----------------

QJsonObject ApiServer::userJson(const DBManager::User &u) const
{
    QJsonObject o;
    o[QStringLiteral("userId")] = static_cast<double>(u.userId);
    o[QStringLiteral("phone")] = u.phone;
    o[QStringLiteral("nickname")] = u.nickname;
    o[QStringLiteral("balance")] = u.balance;
    o[QStringLiteral("status")] = u.status;         // 1=正常 0=冻结
    o[QStringLiteral("statusText")] = u.status == 1 ? QStringLiteral("正常") : QStringLiteral("冻结");
    o[QStringLiteral("registerTime")] = u.registerTime;
    return o;
}

QJsonObject ApiServer::stationJson(const DBManager::Station &s) const
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
    j[QStringLiteral("stationId")] = static_cast<double>(o.stationId);
    j[QStringLiteral("chargerId")] = static_cast<double>(o.chargerId);
    j[QStringLiteral("status")] = o.status;
    j[QStringLiteral("statusText")] = orderStateText(o.status);
    j[QStringLiteral("energy")] = o.energy;
    j[QStringLiteral("amount")] = o.amount;
    j[QStringLiteral("startTime")] = o.startTime;
    j[QStringLiteral("endTime")] = o.endTime;
    return j;
}

// ---------------- C 端接口 ----------------

// POST /api/users/login  手机号免密登录, 不存在自动注册
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

    QJsonObject data;
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

// POST /api/charges {userId, chargerId}  选桩下单(待支付, 电桩已连接)
QHttpServerResponse ApiServer::onCreateCharge(const QHttpServerRequest &req)
{
    QJsonObject body;
    if (!parseBody(req, &body))
        return jsonError(400, QStringLiteral("请求体必须是 JSON"));
    const qint64 userId = static_cast<qint64>(body.value(QStringLiteral("userId")).toDouble());
    const qint64 chargerId = static_cast<qint64>(body.value(QStringLiteral("chargerId")).toDouble());
    if (userId <= 0 || chargerId <= 0)
        return jsonError(400, QStringLiteral("缺少 userId 或 chargerId"));

    QString err;
    qint64 orderId = 0;
    if (!m_db.orderConnect(userId, chargerId, &orderId, &err))
        return jsonError(dbErrorStatus(err), err);

    DBManager::Order o;
    m_db.getOrderById(orderId, &o);
    QJsonObject data;
    data[QStringLiteral("order")] = orderJson(o);
    return jsonCreated(data);
}

// POST /api/charges/<id>/start
QHttpServerResponse ApiServer::onStartCharge(const QHttpServerRequest &req)
{
    const qint64 orderId = pathId(req.url().path(), 3);   // /api/charges/<id>/start
    if (orderId <= 0)
        return jsonError(400, QStringLiteral("订单号无效"));

    QString err;
    if (!m_db.orderStart(orderId, &err))
        return jsonError(dbErrorStatus(err), err);
    DBManager::Order o;
    m_db.getOrderById(orderId, &o);
    return jsonOk(orderJson(o));
}

// POST /api/charges/<id>/finish
QHttpServerResponse ApiServer::onFinishCharge(const QHttpServerRequest &req)
{
    const qint64 orderId = pathId(req.url().path(), 3);
    if (orderId <= 0)
        return jsonError(400, QStringLiteral("订单号无效"));

    QString err;
    if (!m_db.orderFinish(orderId, &err))
        return jsonError(dbErrorStatus(err), err);
    DBManager::Order o;
    m_db.getOrderById(orderId, &o);
    return jsonOk(orderJson(o));
}

// DELETE /api/charges/<id>  取消未开始的订单
QHttpServerResponse ApiServer::onCancelCharge(const QHttpServerRequest &req)
{
    const qint64 orderId = pathId(req.url().path(), 3);   // /api/charges/<id>
    if (orderId <= 0)
        return jsonError(400, QStringLiteral("订单号无效"));

    QString err;
    if (!m_db.orderCancel(orderId, &err))
        return jsonError(dbErrorStatus(err), err);
    QJsonObject data;
    data[QStringLiteral("message")] = QStringLiteral("订单已取消");
    return jsonOk(data);
}

// GET /api/users/<id>/orders
QHttpServerResponse ApiServer::onUserOrders(const QHttpServerRequest &req)
{
    const qint64 userId = pathId(req.url().path(), 3);   // /api/users/<id>/orders
    if (userId <= 0)
        return jsonError(400, QStringLiteral("userId 无效"));

    QJsonArray arr;
    const QVector<DBManager::Order> orders = m_db.listOrders(userId);
    for (const DBManager::Order &o : orders)
        arr.append(orderJson(o));
    QJsonObject data;
    data[QStringLiteral("orders")] = arr;
    return jsonOk(data);
}

// POST /api/users/<id>/recharge {amount}
QHttpServerResponse ApiServer::onRecharge(const QHttpServerRequest &req)
{
    const qint64 userId = pathId(req.url().path(), 3);
    if (userId <= 0)
        return jsonError(400, QStringLiteral("userId 无效"));

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

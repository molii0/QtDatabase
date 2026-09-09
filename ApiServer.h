#ifndef APISERVER_H
#define APISERVER_H

// ============================================================================
// ApiServer —— RESTful JSON 接口服务器(基于官方 QHttpServer)
// ----------------------------------------------------------------------------
// 使用标准 HTTP 方法 + 语义化路由 + 标准状态码(200/201/400/401/403/404/409/500),
// 不再使用自定义负数错误码, 也不再有外层 cmd 包裹。
// 约定:
//   - 成功: 返回资源 JSON; 错误: {"error": "原因"}
//   - 管理端接口需要先登录拿 token, 之后每次请求带请求头
//     Authorization: Bearer <token>
// 详见文档《03-REST接口文档.md》。
// ============================================================================

#include "DBManager.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <QtHttpServer/qhttpserverresponse.h>   // 按值返回用, 需要完整类型

class QHttpServer;
class QHttpServerRequest;
class QTcpServer;

class ApiServer
{
public:
    ApiServer();
    ~ApiServer();
    bool start(quint16 port = 8080);   // 监听并开始服务

private:
    // ---------------- 公共工具(供各 handler 使用) ----------------
    QHttpServerResponse jsonError(int status, const QString &msg);
    QHttpServerResponse jsonOk(const QJsonObject &body);          // 200
    QHttpServerResponse jsonCreated(const QJsonObject &body);     // 201
    // 解析请求体为 JSON; 失败回填 400 响应并返回 false
    bool parseBody(const QHttpServerRequest &req, QJsonObject *out);
    qint64 parseId(const QString &text);                          // 数字字符串 -> id, 非法返回 -1
    // 取 URL 路径第 index 段的数字(如 "/api/users/3/orders" 的第 3 段是 3)
    qint64 pathId(const QString &path, int index);
    // DB 失败信息 -> HTTP 状态码(按文案智能猜测: 冻结403/不存在404/其它冲突409/空500)
    int dbErrorStatus(const QString &msg);
    QString newToken();     // 生成一个随机 token(登录用)
    QHttpServerResponse forbidden(const QString &msg);   // 403 响应

    // JSON 序列化
    QJsonObject userJson(const DBManager::User &u) const;
    QJsonObject stationJson(const DBManager::Station &s, double distanceKm = -1.0) const; // -1 不带距离
    QJsonObject chargerJson(const DBManager::Charger &c) const;
    QJsonObject orderJson(const DBManager::Order &o) const;

    // 管理端鉴权: 校验 Authorization: Bearer <token>; 通过返回 true 并回填账号
    bool requireAdmin(const QHttpServerRequest &req, QString *account);
    // 用户端鉴权: 校验 Bearer token -> 回填 userId
    bool requireUser(const QHttpServerRequest &req, qint64 *userId);
    QHttpServerResponse unauthorized();   // 401 响应(未登录/token 无效)

    // ---------------- C 端(用户)接口 ----------------
    QHttpServerResponse onUserLogin(const QHttpServerRequest &req);
    QHttpServerResponse onListStations(const QHttpServerRequest &req);
    QHttpServerResponse onNearbyStations(const QHttpServerRequest &req);      // 附近电站(距离排序)
    QHttpServerResponse onListChargers(const QHttpServerRequest &req);        // ?stationId=
    QHttpServerResponse onCreateCharge(const QHttpServerRequest &req);        // 选桩下单(待支付)
    QHttpServerResponse onStartCharge(const QHttpServerRequest &req);         // /charges/<id>/start
    QHttpServerResponse onFinishCharge(const QHttpServerRequest &req);        // /charges/<id>/finish
    QHttpServerResponse onCancelCharge(const QHttpServerRequest &req);        // /charges/<id>
    QHttpServerResponse onUserOrders(const QHttpServerRequest &req);          // /users/<id>/orders
    QHttpServerResponse onRecharge(const QHttpServerRequest &req);            // /users/<id>/recharge
    QHttpServerResponse onUpdateProfile(const QHttpServerRequest &req);       // /users/<id>/profile
    QHttpServerResponse onActiveOrder(const QHttpServerRequest &req);         // /users/<id>/active-order

    // ---------------- 管理端接口(均在 ApiServer_admin.cpp) ----------------
    QHttpServerResponse onAdminLogin(const QHttpServerRequest &req);
    QHttpServerResponse onAdminLogout(const QHttpServerRequest &req);
    QHttpServerResponse onAdminListUsers(const QHttpServerRequest &req);
    QHttpServerResponse onAdminSetUserStatus(const QHttpServerRequest &req);
    QHttpServerResponse onAdminStats(const QHttpServerRequest &req);
    QHttpServerResponse onAdminDailyRevenue(const QHttpServerRequest &req);
    QHttpServerResponse onAdminRevenueByStation(const QHttpServerRequest &req);
    QHttpServerResponse onAdminRevenueByCharger(const QHttpServerRequest &req);
    QHttpServerResponse onAdminCreateStation(const QHttpServerRequest &req);
    QHttpServerResponse onAdminUpdateStation(const QHttpServerRequest &req);
    QHttpServerResponse onAdminDeleteStation(const QHttpServerRequest &req);
    QHttpServerResponse onAdminBatchChargers(const QHttpServerRequest &req);
    QHttpServerResponse onAdminDeleteCharger(const QHttpServerRequest &req);
    QHttpServerResponse onAdminChargerAction(const QHttpServerRequest &req);   // fault/recover
    QHttpServerResponse onAdminListOrders(const QHttpServerRequest &req);
    QHttpServerResponse onAdminOpsLogs(const QHttpServerRequest &req);         // /api/admin/logs
    // 演示专用: POST /api/admin/demo/history {days, density}(造大量历史数据, 见 DBManager_demogen.cpp)
    QHttpServerResponse onAdminDemoGen(const QHttpServerRequest &req);

    // 把 DB 结果转成响应(带状态码映射); okData 仅成功时用
    QHttpServerResponse dbResult(bool ok, const QString &err,
                                 const QJsonObject &okData = QJsonObject(),
                                 int okStatus = 200);

    DBManager &m_db;
    QHash<QString, QString> m_tokens;       // token -> 管理员账号
    QHash<QString, qint64> m_userTokens;    // token -> 用户 id
    QHttpServer *m_httpServer = nullptr;    // 在 start() 中创建, dtor 释放
};

#endif // APISERVER_H

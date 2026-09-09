#include "DBManager.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>

// ============================================================================
// 设备接入(设备层 <-> 数据库层)的数据操作
// ----------------------------------------------------------------------------
// Charger Device Simulator(设备层)只与数据库层对接:
//   设备 -> 库: 遥测(charger_telemetry)/心跳(charger_heartbeat)/
//               状态投影(charger.status)
//   库 -> 设备: 命令(device_command, 平台写入, 设备轮询执行后回填)
// 接口层/界面层等其他层不与设备直连, 一律经由数据库层读取这些表。
// ============================================================================

namespace {
// 遥测每桩保留的最大帧数(1 秒 1 帧 = 每桩约 16 分钟历史, 防止表无限膨胀)
constexpr int kTelemetryTrimKeep = 1000;
} // namespace

// ------------------------- 遥测(charger_telemetry) -------------------------

bool DBManager::insertTelemetry(const Telemetry &t, QString *err)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO charger_telemetry (charger_id, ts, status, power, soc, energy, temperature)"
        " VALUES (:cid, :ts, :st, :pw, :soc, :en, :tp);"));
    q.bindValue(QStringLiteral(":cid"), t.chargerId);
    q.bindValue(QStringLiteral(":ts"), t.ts);
    q.bindValue(QStringLiteral(":st"), t.status);
    q.bindValue(QStringLiteral(":pw"), t.power);
    q.bindValue(QStringLiteral(":soc"), t.soc);
    q.bindValue(QStringLiteral(":en"), t.energy);
    q.bindValue(QStringLiteral(":tp"), t.temperature);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("写入遥测失败: %1").arg(q.lastError().text());
        return false;
    }
    return true;
}

bool DBManager::trimTelemetry(qint64 chargerId, int keepRows, QString *err)
{
    if (keepRows <= 0)
        keepRows = kTelemetryTrimKeep;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "DELETE FROM charger_telemetry WHERE charger_id = :cid AND telemetry_id <="
        " (SELECT COALESCE(MAX(telemetry_id), 0) - :keep FROM charger_telemetry"
        "  WHERE charger_id = :cid);"));
    q.bindValue(QStringLiteral(":cid"), chargerId);
    q.bindValue(QStringLiteral(":keep"), keepRows);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("裁剪遥测失败: %1").arg(q.lastError().text());
        return false;
    }
    return true;
}

// ------------------------- 心跳(charger_heartbeat) -------------------------

bool DBManager::upsertHeartbeat(const Heartbeat &h, QString *err)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO charger_heartbeat (charger_id, last_seen, status, uptime_s)"
        " VALUES (:cid, :ls, :st, :up)"
        " ON CONFLICT(charger_id) DO UPDATE SET"
        " last_seen = :ls, status = :st, uptime_s = :up;"));
    q.bindValue(QStringLiteral(":cid"), h.chargerId);
    q.bindValue(QStringLiteral(":ls"), h.lastSeen);
    q.bindValue(QStringLiteral(":st"), h.status);
    q.bindValue(QStringLiteral(":up"), h.uptimeS);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("写入心跳失败: %1").arg(q.lastError().text());
        return false;
    }
    return true;
}

bool DBManager::getHeartbeat(qint64 chargerId, Heartbeat *out) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT charger_id, last_seen, status, uptime_s"
        " FROM charger_heartbeat WHERE charger_id = :cid;"));
    q.bindValue(QStringLiteral(":cid"), chargerId);
    if (!q.exec() || !q.next())
        return false;
    if (out) {
        out->chargerId = q.value(0).toLongLong();
        out->lastSeen  = q.value(1).toString();
        out->status    = q.value(2).toString();
        out->uptimeS   = q.value(3).toLongLong();
    }
    return true;
}

bool DBManager::listHeartbeats(QVector<Heartbeat> *out, QString *err) const
{
    if (!out) {
        if (err) *err = QStringLiteral("listHeartbeats: 输出参数为空");
        return false;
    }
    out->clear();
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral(
            "SELECT charger_id, last_seen, status, uptime_s"
            " FROM charger_heartbeat ORDER BY charger_id;"))) {
        if (err) *err = QStringLiteral("查询心跳失败: %1").arg(q.lastError().text());
        return false;
    }
    while (q.next()) {
        Heartbeat h;
        h.chargerId = q.value(0).toLongLong();
        h.lastSeen  = q.value(1).toString();
        h.status    = q.value(2).toString();
        h.uptimeS   = q.value(3).toLongLong();
        out->append(h);
    }
    return true;
}

// ------------------- 状态投影(charger.status, 4 态业务状态) -------------------
//
// 设备侧 6 态 -> 平台 4 态的映射(由调用方完成, 见模拟器 DeviceDb):
//   idle -> 0空闲  reserved/finished -> 1已连接  charging -> 2充电中  fault -> 3故障
//   offline 不投影: 设备不再上报, 平台按心跳超时判定离线
bool DBManager::syncChargerDeviceStatus(qint64 chargerId, int status, QString *err)
{
    if (status < ChargerIdle || status > ChargerFault) {
        if (err) *err = QStringLiteral("非法的电桩状态: %1").arg(status);
        return false;
    }
    // 该桩有进行中订单(待支付/充电中)时, 状态归订单流程负责, 设备上报不覆盖
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM charging_order WHERE charger_id = :cid AND status IN (0, 1);"));
    q.bindValue(QStringLiteral(":cid"), chargerId);
    if (!q.exec() || !q.next()) {
        if (err) *err = QStringLiteral("查询进行中订单失败");
        return false;
    }
    if (q.value(0).toLongLong() > 0)
        return true;    // 跳过(不是失败): 订单进行中的桩由订单流程管理状态

    q.prepare(QStringLiteral("UPDATE charger SET status = :st WHERE charger_id = :cid;"));
    q.bindValue(QStringLiteral(":st"), status);
    q.bindValue(QStringLiteral(":cid"), chargerId);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("同步设备状态失败: %1").arg(q.lastError().text());
        return false;
    }
    return true;
}

// ------------------------- 命令通道(device_command) -------------------------

bool DBManager::pushDeviceCommand(qint64 chargerId, const QString &command, const QString &arg,
                                  qint64 *newCommandId, QString *err)
{
    Charger c;
    if (!getCharger(chargerId, &c)) {
        if (err) *err = QStringLiteral("电桩不存在 (charger_id=%1)").arg(chargerId);
        return false;
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO device_command (charger_id, command, arg) VALUES (:cid, :cmd, :arg);"));
    q.bindValue(QStringLiteral(":cid"), chargerId);
    q.bindValue(QStringLiteral(":cmd"), command.trimmed().toLower());
    q.bindValue(QStringLiteral(":arg"), arg.isNull() ? QStringLiteral("") : arg);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("下发设备命令失败: %1").arg(q.lastError().text());
        return false;
    }
    if (newCommandId)
        *newCommandId = q.lastInsertId().toLongLong();
    return true;
}

bool DBManager::takePendingDeviceCommands(qint64 chargerId, QVector<DeviceCommand> *out,
                                          QString *err) const
{
    if (!out) {
        if (err) *err = QStringLiteral("takePendingDeviceCommands: 输出参数为空");
        return false;
    }
    out->clear();
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT command_id, charger_id, command, arg, status, result, created_at, done_at"
        " FROM device_command WHERE charger_id = :cid AND status = 0 ORDER BY command_id;"));
    q.bindValue(QStringLiteral(":cid"), chargerId);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("查询待执行命令失败: %1").arg(q.lastError().text());
        return false;
    }
    while (q.next()) {
        DeviceCommand c;
        c.commandId = q.value(0).toLongLong();
        c.chargerId = q.value(1).toLongLong();
        c.command   = q.value(2).toString();
        c.arg       = q.value(3).toString();
        c.status    = q.value(4).toInt();
        c.result    = q.value(5).toString();
        c.createdAt = q.value(6).toString();
        c.doneAt    = q.value(7).toString();
        out->append(c);
    }
    return true;
}

bool DBManager::finishDeviceCommand(qint64 commandId, bool ok, const QString &result,
                                    QString *err)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "UPDATE device_command SET status = :st, result = :rs, done_at = :dt"
        " WHERE command_id = :id;"));
    q.bindValue(QStringLiteral(":st"), ok ? 1 : 2);
    q.bindValue(QStringLiteral(":rs"), result.isNull() ? QStringLiteral("") : result);
    q.bindValue(QStringLiteral(":dt"), nowStr());
    q.bindValue(QStringLiteral(":id"), commandId);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("回填命令结果失败: %1").arg(q.lastError().text());
        return false;
    }
    if (q.numRowsAffected() == 0) {
        if (err) *err = QStringLiteral("命令不存在 (command_id=%1)").arg(commandId);
        return false;
    }
    return true;
}

bool DBManager::listDeviceCommands(int limit, QVector<DeviceCommand> *out, QString *err) const
{
    if (!out) {
        if (err) *err = QStringLiteral("listDeviceCommands: 输出参数为空");
        return false;
    }
    out->clear();
    QSqlQuery q(db());
    if (limit < 0) {
        q.prepare(QStringLiteral(
            "SELECT command_id, charger_id, command, arg, status, result, created_at, done_at"
            " FROM device_command ORDER BY command_id DESC;"));
    } else {
        q.prepare(QStringLiteral(
            "SELECT command_id, charger_id, command, arg, status, result, created_at, done_at"
            " FROM device_command ORDER BY command_id DESC LIMIT :lim;"));
        q.bindValue(QStringLiteral(":lim"), limit);
    }
    if (!q.exec()) {
        if (err) *err = QStringLiteral("查询设备命令失败: %1").arg(q.lastError().text());
        return false;
    }
    while (q.next()) {
        DeviceCommand c;
        c.commandId = q.value(0).toLongLong();
        c.chargerId = q.value(1).toLongLong();
        c.command   = q.value(2).toString();
        c.arg       = q.value(3).toString();
        c.status    = q.value(4).toInt();
        c.result    = q.value(5).toString();
        c.createdAt = q.value(6).toString();
        c.doneAt    = q.value(7).toString();
        out->append(c);
    }
    return true;
}

// ============================================================
// DeviceDb 实现(见 devicedb.h)
// ============================================================
#include "devicedb.h"

#include <QDateTime>
#include <QtGlobal>

// 遥测每桩保留帧数(1 秒 1 帧), 到量后由 DBManager::trimTelemetry 裁剪旧帧
static constexpr int kTrimEveryFrames = 120;

bool DeviceDb::open(const QString &dbPath, QString *err)
{
    // DBManager::init 会建表/迁移/补索引, 空库自动写演示数据
    if (!DBManager::instance().init(dbPath)) {
        if (err) *err = QStringLiteral("数据库初始化失败: %1").arg(dbPath);
        return false;
    }
    return true;
}

QVector<DBManager::Charger> DeviceDb::pickChargers(qint64 stationId, int maxCount,
                                                   QString *err) const
{
    QVector<DBManager::Charger> all = DBManager::instance().listChargers(stationId);
    if (all.isEmpty() && err)
        *err = QStringLiteral("数据库中没有可模拟的电桩");
    if (maxCount > 0 && all.size() > maxCount)
        all.resize(maxCount);
    return all;
}

// ------------------------- 设备 -> 库 -------------------------

bool DeviceDb::pushTelemetry(const Charger &c, const QString &ts, QString *err)
{
    DBManager::Telemetry t;
    t.chargerId   = c.id();
    t.ts          = ts;
    t.status      = c.stateName();
    t.power       = c.powerKw();
    t.soc         = c.soc();
    t.energy      = c.energyKwh();
    t.temperature = c.temperatureC();
    if (!DBManager::instance().insertTelemetry(t, err))
        return false;
    ++m_telemetryFrames;
    trimIfDue(c);
    return true;
}

bool DeviceDb::pushHeartbeat(const Charger &c, const QString &ts, QString *err)
{
    DBManager::Heartbeat h;
    h.chargerId = c.id();
    h.lastSeen  = ts;
    h.status    = c.stateName();
    h.uptimeS   = c.uptimeS();
    if (!DBManager::instance().upsertHeartbeat(h, err))
        return false;
    ++m_heartbeatFrames;
    return true;
}

bool DeviceDb::pushStatus(const Charger &c, QString *err)
{
    const int projected = projectStatus(c.state());
    if (projected < 0)
        return true;    // offline 不投影: 设备已"失联", 平台按心跳超时判定
    return DBManager::instance().syncChargerDeviceStatus(c.id(), projected, err);
}

bool DeviceDb::logDeviceEvent(const Charger &c, const QString &action, const QString &detail,
                              QString *err)
{
    if (!DBManager::instance().addOpsLog(QStringLiteral("device"), c.id(), c.code(),
                                         action, detail)) {
        if (err) *err = QStringLiteral("写运维日志失败");
        return false;
    }
    return true;
}

void DeviceDb::trimIfDue(const Charger &c)
{
    const qint64 id = c.id();
    int left = m_trimCountdown.value(id, kTrimEveryFrames) - 1;
    if (left <= 0) {
        DBManager::instance().trimTelemetry(id, 1000);
        left = kTrimEveryFrames;
    }
    m_trimCountdown.insert(id, left);
}

// ------------------------- 库 -> 设备 -------------------------

QVector<DBManager::DeviceCommand> DeviceDb::pollCommands(int chargerId, QString *err)
{
    QVector<DBManager::DeviceCommand> cmds;
    if (!DBManager::instance().takePendingDeviceCommands(chargerId, &cmds, err))
        cmds.clear();
    return cmds;
}

bool DeviceDb::ackCommand(qint64 commandId, bool ok, const QString &result, QString *err)
{
    return DBManager::instance().finishDeviceCommand(commandId, ok, result, err);
}

// ------------------------- 内部 -------------------------

int DeviceDb::projectStatus(Charger::State s)
{
    switch (s) {
    case Charger::Idle:     return ChargerIdle;      // 0 空闲
    case Charger::Reserved: return ChargerConnected; // 1 已连接
    case Charger::Charging: return ChargerCharging;  // 2 充电中
    case Charger::Finished: return ChargerConnected; // 1 已连接(枪还插着)
    case Charger::Fault:    return ChargerFault;     // 3 故障
    case Charger::Offline:  break;                   // 不投影
    }
    return -1;
}

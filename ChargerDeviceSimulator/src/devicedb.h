#pragma once
// ============================================================
// DeviceDb —— 设备层 <-> 数据库层的桥(模拟器唯一的对外通道)
// ------------------------------------------------------------
// 架构约定(见 README/需求): 模拟设备只与"数据库层"对接, 不与接口层/
// 界面层等其他层直连; 其他层的数据一律经由数据库层。
//
//   设备 -> 库(每秒/每 5 秒/状态变化时):
//     pushTelemetry   遥测帧 -> charger_telemetry
//     pushHeartbeat   心跳   -> charger_heartbeat(upsert, 平台按
//                     now - last_seen 超过阈值判定设备离线)
//     pushStatus      设备状态投影 -> charger.status(4 态业务状态;
//                     桩上有进行中订单时由 DBManager 跳过, 归订单流程)
//   库 -> 设备(每秒轮询):
//     pollCommands    device_command 里平台下发的待执行命令
//
// 其中 pushStatus 的状态映射(设备 6 态 -> 平台 4 态):
//   idle -> 0空闲   reserved/finished -> 1已连接
//   charging -> 2充电中   fault -> 3故障   offline -> 不投影(心跳停更)
// ============================================================

#include <QHash>
#include <QString>
#include <QVector>

#include "charger.h"
#include "DBManager.h"

class DeviceDb
{
public:
    // 打开数据库(文件不存在会自动建库并写演示数据); 失败时 err 给出原因
    bool open(const QString &dbPath, QString *err = nullptr);
    QString dbPath() const { return DBManager::instance().dbPath(); }

    // 领取要模拟的电桩: stationId<0 不限电站; maxCount<=0 不限数量。
    // 返回空且 err 为空表示库里有桩但被条件过滤掉了
    QVector<DBManager::Charger> pickChargers(qint64 stationId, int maxCount,
                                             QString *err = nullptr) const;

    // ---- 设备 -> 库 ----
    bool pushTelemetry(const Charger &c, const QString &ts, QString *err = nullptr);
    bool pushHeartbeat(const Charger &c, const QString &ts, QString *err = nullptr);
    bool pushStatus(const Charger &c, QString *err = nullptr);   // 状态投影(offline 跳过)
    // 运维日志(设备在线时可上报: 故障/恢复); adminAccount 用 "device"
    bool logDeviceEvent(const Charger &c, const QString &action, const QString &detail,
                        QString *err = nullptr);
    // 遥测裁剪: 每桩只保留最近若干帧; 由 pushTelemetry 内部计数到量自动触发
    void trimIfDue(const Charger &c);

    // ---- 库 -> 设备 ----
    QVector<DBManager::DeviceCommand> pollCommands(int chargerId, QString *err = nullptr);
    bool ackCommand(qint64 commandId, bool ok, const QString &result, QString *err = nullptr);

    // 已推送帧计数(退出时打印, 也算设备的"发送统计")
    qint64 telemetryFrames() const { return m_telemetryFrames; }
    qint64 heartbeatFrames() const { return m_heartbeatFrames; }

private:
    static int projectStatus(Charger::State s);   // 设备 6 态 -> 平台 4 态; offline 返回 -1

    qint64 m_telemetryFrames = 0;
    qint64 m_heartbeatFrames = 0;
    QHash<qint64, int> m_trimCountdown;           // 每桩距下次遥测裁剪的帧数
};

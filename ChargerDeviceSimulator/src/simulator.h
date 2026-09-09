#pragma once
// ============================================================
// Simulator —— 多设备模拟器(设备侧管理器)
// ------------------------------------------------------------
// v2: 持有 N 台 Charger(与数据库里的电桩一一绑定), 用一个 1 秒定时器
// 驱动它们"自运行", 并把设备数据全部送进数据库层(唯一的对接层):
//   * 每秒  : 在网设备遥测 [TELEM] -> charger_telemetry 表
//   * 每 5 秒: 在网设备心跳 [HEART] -> charger_heartbeat 表(平台按
//             last_seen 超时判定设备离线)
//   * 状态变化: [EVENT] 打印 + 状态投影 charger.status + 故障运维日志
//   * 每秒轮询 device_command 表, 执行平台下发的命令并回填 [ACK]
// 控制台输入仍可作为"本地物理操作"(插枪/启动/故障模拟等)。
// --auto 打开后设备随机插枪充电/随机故障/随机掉线并自动恢复, 免手打演示。
// ============================================================

#include <QList>
#include <QString>
#include <QTimer>
#include <QVector>

#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "charger.h"
#include "devicedb.h"

struct SimulatorOptions {
    int      deviceCount = 3;    // 最多绑定的电桩数(从数据库按序领取)
    int      durationSec = -1;   // >0: 运行 N 秒后自动退出
    QString  dbPath;             // 数据库文件(空=由 main 按默认规则解析)
    qint64   stationId   = -1;   // >=0: 只模拟该电站下的电桩
    bool     autoMode    = false;// 随机场景自动演示
};

class Simulator
{
public:
    explicit Simulator(const SimulatorOptions &options);
    ~Simulator();

    bool init(QString *err = nullptr);   // 打开数据库并绑定要模拟的电桩
    void start();                        // 启动定时器与控制台线程

private:
    void onTick();                 // 每秒: 命令 -> 自运行 -> 事件入库 -> 心跳/遥测
    void drainCommands();          // 处理积压的控制台命令
    void handleCommand(const QString &line);
    // 统一执行一个设备动作(console=本地控制台 / server=平台命令 / auto=自动演示)
    bool execAction(int id, const QString &action, const QString &arg,
                    const QString &source, QString *errOut = nullptr);
    void pollDbCommands();         // 轮询 device_command(库 -> 设备)
    void runAutoStep();            // --auto: 随机场景
    void handleEvents();           // [EVENT] 打印 + 状态投影/运维日志入库
    void pushTelemetryOf(Charger &c);
    void pushHeartbeatOf(Charger &c);

    Charger *findDevice(int id);
    void printStatusTable();
    void printDbInfo();

    SimulatorOptions m_opt;
    DeviceDb m_db;

    QList<Charger> m_devices;
    QVector<Charger::State> m_lastPushed;   // 每台设备上次入库的状态(离线也记录)

    // --auto 模式下每台设备的自动动作倒计时(秒)
    struct AutoCtl {
        int startInS   = 0;   // 插枪后自动开始充电的倒计时
        int recoverInS = 0;   // 故障自动恢复倒计时
        int onlineInS  = 0;   // 掉线自动上线倒计时
    };
    QVector<AutoCtl> m_auto;

    QTimer m_tickTimer;
    int    m_tick = 0;
    bool   m_quitRequested = false;
    bool   m_telemWarned   = false;   // 入库失败只告警一次, 恢复后复位
    bool   m_heartWarned   = false;

    std::queue<std::string> m_cmdQueue;
    std::mutex m_mtx;         // 保护 m_cmdQueue
    std::thread m_stdinThread;
};

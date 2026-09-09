#pragma once
// ============================================================
// Charger —— 单个模拟充电桩（设备侧 / Device Side）
// ------------------------------------------------------------
// 与 Database 平台完全独立：不连数据库、不依赖平台任何代码。
// 它模拟"一台真实设备"自己该有的东西：
//   状态机 / SoC / 功率 / 电量 / 温度 / 故障 / 掉线 / 重启
//
// 状态机（见 需求.md）：
//   IDLE -> RESERVED -> CHARGING -> FINISHED -> IDLE
//   任意状态 -> FAULT -> RECOVER -> IDLE
//   任意状态 -> OFFLINE -> ONLINE -> IDLE
//   restart: 任意状态 -> OFFLINE ->(约2秒自动)-> IDLE
// ============================================================

#include <QString>
#include <QStringList>

class Charger
{
public:
    enum State {
        Offline   = 0,   // 掉线/断电（停止遥测与心跳）
        Idle      = 1,   // 空闲，等待插枪
        Reserved  = 2,   // 已插枪/已预约，未开始充电
        Charging  = 3,   // 充电中
        Finished  = 4,   // 本次充电结束，等待拔枪
        Fault     = 5    // 故障
    };

    // initSocPct: 初始电量(%); capacityKwh: 电池容量; maxPowerKw: 额定功率;
    // simScale: 演示加速比（1 现实秒 = simScale 模拟秒，默认 60，便于快速看到效果）
    // code: 设备编号展示名; 留空则用 CHG-<id>(对接数据库后传桩号, 如 DR-01)
    Charger(int id, double initSocPct, double capacityKwh, double maxPowerKw,
            double simScale = 60.0, const QString &code = QString());

    // ---- 只读状态 ----
    int    id()          const { return m_id; }
    QString code()       const { return m_code; }        // 设备编号, 如 CHG-001
    State  state()       const { return m_state; }
    QString stateName()  const { return stateName(m_state); }
    static QString stateName(State s);                   // 小写英文名(status字段用)

    double soc()         const { return m_soc; }         // 电池电量 %
    double powerKw()     const;                          // 当前输出功率 kW（不充电为 0）
    double energyKwh()   const { return m_energyKwh; }   // 本次充电已累计电量 kWh
    double temperatureC() const { return m_tempC; }      // 温度 ℃
    int    faultCode()   const { return m_faultCode; }   // 故障码（0=无故障）
    long   uptimeS()     const { return m_uptimeS; }     // 已运行秒数（重启清零）

    // ---- 设备命令（将来由 Server -> Device 下发；本版由控制台输入模拟下发）----
    // 每项返回是否成功；失败时 err 给出原因。
    bool plug    (QString *err = nullptr);               // IDLE      -> RESERVED
    bool start   (QString *err = nullptr);               // RESERVED  -> CHARGING
    bool stop    (QString *err = nullptr);               // CHARGING  -> FINISHED(手动结束)
    bool unplug  (QString *err = nullptr);               // FINISHED  -> IDLE
    bool fault   (int code, QString *err = nullptr);     // -> FAULT
    bool recover (QString *err = nullptr);               // FAULT     -> IDLE
    bool offline (QString *err = nullptr);               // -> OFFLINE(模拟掉线/断电)
    bool online  (QString *err = nullptr);               // OFFLINE   -> IDLE
    bool restart (QString *err = nullptr);               // 重启: OFFLINE ->(2秒)-> IDLE

    // ---- 设备"自运行" ----
    // 由上层定时器每秒调用一次（dtSec=1）。设备自己推进：充电时 soc/电量上升、
    // 温度随 soc 升高；充满自动转 FINISHED；拔枪延时后自动回 IDLE（不依赖任何人）。
    void tick(double dtSec);

    // 取走并清空本秒内产生的全部状态变化描述(用于上层打印 [EVENT])
    QStringList takeEvents();

private:
    bool requireState(State s, const char *cmd, QString *err) const;
    void changeState(State to, const QString &note = QString());
    void driftTemperature(double targetC, double dtSec);

    int    m_id;
    QString m_code;
    State  m_state    = Idle;
    double m_soc      = 0.0;
    double m_capacityKwh = 0.0;
    double m_maxPowerKw  = 0.0;
    double m_simScale    = 60.0;      // 演示加速比
    double m_energyKwh   = 0.0;
    double m_tempC       = 25.0;
    int    m_faultCode   = 0;
    long   m_uptimeS     = 0;

    int m_bootWaitS   = 0;   // 重启/开机剩余秒数
    int m_finishWaitS = 0;   // FINISHED 后等待拔枪剩余秒数
    QStringList m_events;    // 本秒内状态变化记录(可能多条)
};

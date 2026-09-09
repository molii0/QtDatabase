// ============================================================
// Charger 实现（见 charger.h）
// ============================================================
#include "charger.h"
#include <QtGlobal>

Charger::Charger(int id, double initSocPct, double capacityKwh,
                 double maxPowerKw, double simScale, const QString &code)
    : m_id(id)
    , m_code(code.isEmpty()
                 ? QStringLiteral("CHG-%1").arg(id, 3, 10, QLatin1Char('0'))
                 : code)
    , m_soc(initSocPct)
    , m_capacityKwh(capacityKwh)
    , m_maxPowerKw(maxPowerKw)
    , m_simScale(simScale)
{
    m_soc = qBound(0.0, m_soc, 100.0);
}

QString Charger::stateName(State s)
{
    switch (s) {
    case Offline:   return QStringLiteral("offline");
    case Idle:      return QStringLiteral("idle");
    case Reserved:  return QStringLiteral("reserved");
    case Charging:  return QStringLiteral("charging");
    case Finished:  return QStringLiteral("finished");
    case Fault:     return QStringLiteral("fault");
    }
    return QStringLiteral("unknown");
}

double Charger::powerKw() const
{
    return m_state == Charging ? m_maxPowerKw : 0.0;
}

// ------------------------------------------------------------
// 设备命令：每个命令先做"原状态校验"，再走状态机
// ------------------------------------------------------------
bool Charger::requireState(State s, const char *cmd, QString *err) const
{
    if (m_state != s) {
        if (err) {
            *err = QStringLiteral("%1: 需要状态 %2, 当前 %3")
                       .arg(QLatin1String(cmd), stateName(s), stateName());
        }
        return false;
    }
    return true;
}

void Charger::changeState(State to, const QString &note)
{
    QString ev = stateName() + QStringLiteral(" -> ") + stateName(to);
    if (!note.isEmpty())
        ev += QStringLiteral(" (") + note + QLatin1Char(')');
    m_events.append(ev);
    m_state = to;
    if (to == Finished)
        m_finishWaitS = 0;            // 进入 FINISHED 时开始"拔枪倒计时"
}

bool Charger::plug(QString *err)      // IDLE -> RESERVED
{
    if (!requireState(Idle, "plug", err)) return false;
    changeState(Reserved, QStringLiteral("plugged"));
    return true;
}

bool Charger::start(QString *err)     // RESERVED -> CHARGING
{
    if (!requireState(Reserved, "start", err)) return false;
    if (m_soc >= 99.99) {
        if (err) *err = QStringLiteral("start: 电池已充满, 无需充电");
        return false;
    }
    m_energyKwh = 0.0;                // 本次充电电量从 0 累计
    changeState(Charging, QStringLiteral("session started"));
    return true;
}

bool Charger::stop(QString *err)      // CHARGING -> FINISHED(手动结束)
{
    if (!requireState(Charging, "stop", err)) return false;
    changeState(Finished, QStringLiteral("stopped by user"));
    return true;
}

bool Charger::unplug(QString *err)    // FINISHED -> IDLE
{
    if (!requireState(Finished, "unplug", err)) return false;
    m_energyKwh = 0.0;
    changeState(Idle, QStringLiteral("unplugged"));
    return true;
}

bool Charger::fault(int code, QString *err)   // 任意状态 -> FAULT
{
    if (m_state == Fault) {
        if (err) *err = QStringLiteral("fault: 设备已在故障状态");
        return false;
    }
    if (m_state == Offline) {
        if (err) *err = QStringLiteral("fault: 设备已掉线, 无法上报故障");
        return false;
    }
    m_faultCode = code <= 0 ? 1 : code;
    changeState(Fault, QStringLiteral("fault code %1").arg(m_faultCode));
    return true;
}

bool Charger::recover(QString *err)   // FAULT -> IDLE
{
    if (!requireState(Fault, "recover", err)) return false;
    m_faultCode = 0;
    m_energyKwh = 0.0;                // 故障中止了本次充电, 会话电量清零
    changeState(Idle, QStringLiteral("recovered"));
    return true;
}

bool Charger::offline(QString *err)   // 任意状态 -> OFFLINE(模拟掉线/断电)
{
    if (m_state == Offline) {
        if (err) *err = QStringLiteral("offline: 设备已在掉线状态");
        return false;
    }
    m_bootWaitS = 0;
    changeState(Offline, QStringLiteral("network down"));
    return true;
}

bool Charger::online(QString *err)    // OFFLINE -> IDLE
{
    if (!requireState(Offline, "online", err)) return false;
    m_bootWaitS = 0;
    m_energyKwh = 0.0;                // 断电丢失会话现场, 电量清零
    changeState(Idle, QStringLiteral("online again"));
    return true;
}

bool Charger::restart(QString *err)   // 重启: OFFLINE ->(约2秒自动)-> IDLE
{
    Q_UNUSED(err);
    m_faultCode   = 0;                // 上电自检清零故障
    m_energyKwh   = 0.0;
    m_uptimeS     = 0;
    m_bootWaitS   = 2;
    if (m_state != Offline)
        changeState(Offline, QStringLiteral("restart requested"));
    else
        m_events.append(QStringLiteral("offline -> offline (restart requested)"));
    return true;
}

// ------------------------------------------------------------
// 设备"自运行"：由上层每秒调用一次
// ------------------------------------------------------------
void Charger::tick(double dtSec)
{
    ++m_uptimeS;

    switch (m_state) {
    case Charging: {
        // 1 现实秒 = m_simScale 模拟秒；功率 m_maxPowerKw kW
        const double simSec   = dtSec * m_simScale;
        const double dEnergy  = m_maxPowerKw / 3600.0 * simSec;   // kWh
        m_energyKwh += dEnergy;
        m_soc = qMin(100.0, m_soc + dEnergy / m_capacityKwh * 100.0);
        // 温度随 soc 缓慢升高，模拟真实充电发热
        driftTemperature(28.0 + m_soc * 0.15, dtSec);
        if (m_soc >= 99.999) {          // 充满：自动转 FINISHED
            m_soc = 100.0;
            changeState(Finished, QStringLiteral("soc reached 100%"));
        }
        break;
    }
    case Finished:
        // 充满/手动结束后"拔枪倒计时"，到点自动回 IDLE
        if (++m_finishWaitS >= 3) {
            m_finishWaitS = 0;
            m_energyKwh   = 0.0;
            changeState(Idle, QStringLiteral("auto unplug"));
        }
        driftTemperature(25.0, dtSec);
        break;
    case Offline:
        // 重启后的"开机"过程
        if (m_bootWaitS > 0 && --m_bootWaitS == 0)
            changeState(Idle, QStringLiteral("boot complete"));
        break;
    case Idle:
    case Reserved:
    case Fault:
    default:
        driftTemperature(25.0, dtSec);  // 不充电时温度缓慢回到环境温度
        break;
    }
}

void Charger::driftTemperature(double targetC, double dtSec)
{
    const double k = qMin(1.0, dtSec / 20.0);   // 约 20 秒趋近目标
    m_tempC += (targetC - m_tempC) * k;
}

QStringList Charger::takeEvents()
{
    const QStringList evs = m_events;
    m_events.clear();
    return evs;
}
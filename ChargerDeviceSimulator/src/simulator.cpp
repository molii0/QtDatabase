// ============================================================
// Simulator 实现(见 simulator.h)
// ============================================================
#include "simulator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRandomGenerator>
#include <QTextStream>
#include <QtGlobal>

#include <cmath>
#include <iostream>
#include <mutex>

// 控制台输出流：每次写完即 flush，保证重定向/管道也能实时看到
static QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

static double round1(double v) { return std::round(v * 10.0) / 10.0; }
static double round2(double v) { return std::round(v * 100.0) / 100.0; }

static QString nowStr()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

Simulator::Simulator(const SimulatorOptions &options)
    : m_opt(options)
{
}

Simulator::~Simulator()
{
    m_tickTimer.stop();
}

// ------------------------------------------------------------
// 初始化: 打开数据库并绑定要模拟的电桩(设备 id = charger_id)
// ------------------------------------------------------------
bool Simulator::init(QString *err)
{
    QString dbErr;
    if (!m_db.open(m_opt.dbPath, &dbErr)) {
        if (err) *err = dbErr;
        return false;
    }

    QString pickErr;
    const QVector<DBManager::Charger> rows =
        m_db.pickChargers(m_opt.stationId, m_opt.deviceCount, &pickErr);
    if (rows.isEmpty()) {
        if (err) *err = pickErr.isEmpty() ? QStringLiteral("数据库中没有可模拟的电桩") : pickErr;
        return false;
    }

    // 电池容量演示固定 60kWh; 额定功率取数据库里该桩的功率(快充/慢充自然区分)
    for (int i = 0; i < rows.size(); ++i) {
        const DBManager::Charger &row = rows.at(i);
        const double initSoc = 20.0 + (i % 5) * 15.0;      // 20/35/50/65/80 …
        const double power   = row.power > 0 ? row.power : 60.0;
        m_devices.append(Charger(static_cast<int>(row.chargerId), initSoc, 60.0, power,
                                 60.0, row.code));
    }
    m_lastPushed.assign(m_devices.size(), Charger::Idle);
    m_auto.resize(m_devices.size());

    // 启动时把初始状态(空闲)投影一次到 charger.status
    for (const Charger &c : m_devices) {
        QString e;
        if (!m_db.pushStatus(c, &e))
            out() << "[WARN] 初始状态入库失败: " << e << Qt::endl;
    }
    return true;
}

void Simulator::start()
{
    out() << "[INFO] ChargerSimulator v2 (device side, storage = database layer)"
          << Qt::endl;
    out() << "[INFO] db=" << m_db.dbPath()
          << "  devices=" << m_devices.size()
          << "  station=" << (m_opt.stationId >= 0 ? QString::number(m_opt.stationId)
                                                    : QStringLiteral("all"));
    if (m_opt.autoMode)
        out() << "  auto=on";
    if (m_opt.durationSec > 0)
        out() << "  autoStop=" << m_opt.durationSec << "s";
    out() << Qt::endl;
    out() << "[INFO] tick=1s heartbeat=5s simScale=60 (1s real = 1min sim)" << Qt::endl;
    printStatusTable();
    out() << "[INFO] type 'help' to see commands ('db' shows storage info)" << Qt::endl;

    // 后台线程读控制台命令 -> 命令队列（主循环每秒取一次）
    m_stdinThread = std::thread([this] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_cmdQueue.push(line);
        }
    });
    m_stdinThread.detach();   // 不 join：exit 时进程结束即可

    // 1 秒驱动一次所有设备
    QObject::connect(&m_tickTimer, &QTimer::timeout, [this] { onTick(); });
    m_tickTimer.start(1000);

    // 限时自动退出
    if (m_opt.durationSec > 0) {
        QTimer::singleShot(m_opt.durationSec * 1000, [] {
            QCoreApplication *app = QCoreApplication::instance();
            if (app) app->quit();
        });
    }

    // 退出时统一打印设备"发送统计"(exit 命令与限时退出都经过这里)
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [this] {
        out() << "[INFO] session over: telemetry frames=" << m_db.telemetryFrames()
              << " heartbeat frames=" << m_db.heartbeatFrames()
              << " (db=" << m_db.dbPath() << ")" << Qt::endl;
    });
}

void Simulator::onTick()
{
    ++m_tick;

    drainCommands();              // 1. 本地控制台命令
    pollDbCommands();             // 2. 平台经数据库下发的命令
    runAutoStep();                // 3. --auto 随机场景

    // 4. 设备"自运行"一秒
    for (Charger &c : m_devices)
        c.tick(1.0);

    // 5. 状态变化事件 -> 打印 + 入库(状态投影/运维日志)
    handleEvents();

    // 6. 心跳（每 5 秒一次；掉线设备不发）
    if (m_tick % 5 == 0) {
        for (Charger &c : m_devices) {
            if (c.state() == Charger::Offline) continue;
            QJsonObject o;
            o.insert(QStringLiteral("device_id"), c.id());
            o.insert(QStringLiteral("ts"), nowStr());
            o.insert(QStringLiteral("status"), c.stateName());
            o.insert(QStringLiteral("uptime_s"), static_cast<qint64>(c.uptimeS()));
            out() << "[HEART] " << QJsonDocument(o).toJson(QJsonDocument::Compact)
                  << Qt::endl;
            pushHeartbeatOf(c);
        }
    }

    // 7. 遥测（每秒一次；掉线设备不发）
    for (Charger &c : m_devices) {
        if (c.state() == Charger::Offline) continue;
        QJsonObject o;
        o.insert(QStringLiteral("device_id"), c.id());
        o.insert(QStringLiteral("ts"), nowStr());
        o.insert(QStringLiteral("status"), c.stateName());
        o.insert(QStringLiteral("power"), round1(c.powerKw()));
        o.insert(QStringLiteral("soc"), round1(c.soc()));
        o.insert(QStringLiteral("energy"), round2(c.energyKwh()));
        o.insert(QStringLiteral("temperature"), round1(c.temperatureC()));
        out() << "[TELEM] " << QJsonDocument(o).toJson(QJsonDocument::Compact)
              << Qt::endl;
        pushTelemetryOf(c);
    }

    if (m_quitRequested) {
        out() << "[INFO] bye" << Qt::endl;
        QCoreApplication *app = QCoreApplication::instance();
        if (app) app->quit();
    }
}

// 入库失败(如库被长时间锁住)只告警一次, 恢复后复位, 避免刷屏
void Simulator::pushTelemetryOf(Charger &c)
{
    QString e;
    if (m_db.pushTelemetry(c, nowStr(), &e)) {
        m_telemWarned = false;
        return;
    }
    if (!m_telemWarned) {
        out() << "[WARN] 遥测入库失败(不再重复提示): " << e << Qt::endl;
        m_telemWarned = true;
    }
}

void Simulator::pushHeartbeatOf(Charger &c)
{
    QString e;
    if (m_db.pushHeartbeat(c, nowStr(), &e)) {
        m_heartWarned = false;
        return;
    }
    if (!m_heartWarned) {
        out() << "[WARN] 心跳入库失败(不再重复提示): " << e << Qt::endl;
        m_heartWarned = true;
    }
}

// ------------------------------------------------------------
// 状态变化: 打印 [EVENT], 并把状态投影入库; 故障/恢复写运维日志
// (离线本身不入库 —— 设备已"失联", 平台按心跳超时判定离线)
// ------------------------------------------------------------
void Simulator::handleEvents()
{
    for (int i = 0; i < m_devices.size(); ++i) {
        Charger &c = m_devices[i];
        const QStringList evs = c.takeEvents();
        if (evs.isEmpty())
            continue;

        for (const QString &ev : evs)
            out() << "[EVENT] device " << c.id() << ": " << ev << Qt::endl;

        if (c.state() == m_lastPushed.at(i))
            continue;   // 如"已离线时再 restart": 状态没变, 无需同步

        QString e;
        if (!m_db.pushStatus(c, &e))
            out() << "[WARN] 状态入库失败: " << e << Qt::endl;

        if (c.state() == Charger::Fault) {
            m_db.logDeviceEvent(c, QStringLiteral("设备故障上报"),
                                QStringLiteral("故障码 %1").arg(c.faultCode()));
        } else if (m_lastPushed.at(i) == Charger::Fault) {
            m_db.logDeviceEvent(c, QStringLiteral("设备故障恢复"),
                                QStringLiteral("%1 恢复正常").arg(c.code()));
        }
        m_lastPushed[i] = c.state();
    }
}

// ------------------------------------------------------------
// 库 -> 设备: 轮询平台下发的命令并执行, 执行后回填 [ACK]
// (掉线设备收不到命令, 命令保持待执行, 重新上线后补执行)
// ------------------------------------------------------------
void Simulator::pollDbCommands()
{
    for (Charger &c : m_devices) {
        if (c.state() == Charger::Offline) continue;
        QString err;
        const QVector<DBManager::DeviceCommand> cmds = m_db.pollCommands(c.id(), &err);
        if (!err.isEmpty()) {
            out() << "[WARN] 命令轮询失败: " << err << Qt::endl;
            continue;
        }
        for (const DBManager::DeviceCommand &cmd : cmds) {
            out() << "[CMD ] #" << cmd.commandId << " server -> device " << c.id()
                  << ": " << cmd.command
                  << (cmd.arg.isEmpty() ? QString() : QStringLiteral(" %1").arg(cmd.arg))
                  << Qt::endl;
            QString execErr;
            const bool ok = execAction(c.id(), cmd.command, cmd.arg,
                                       QStringLiteral("server"), &execErr);
            m_db.ackCommand(cmd.commandId, ok, ok ? QStringLiteral("ok") : execErr, &err);
        }
    }
}

// ------------------------------------------------------------
// --auto: 随机场景(插枪充电/随机故障/随机掉线), 全部自动恢复
// ------------------------------------------------------------
void Simulator::runAutoStep()
{
    if (!m_opt.autoMode)
        return;
    QRandomGenerator &rng = *QRandomGenerator::global();

    for (int i = 0; i < m_devices.size(); ++i) {
        Charger &c = m_devices[i];
        AutoCtl &a = m_auto[i];

        switch (c.state()) {
        case Charger::Idle:
            if (a.startInS > 0) {                       // 插枪后稍等自动开始充电
                if (--a.startInS == 0)
                    execAction(c.id(), QStringLiteral("start"), QString(), QStringLiteral("auto"));
            } else if (rng.bounded(100) < 3) {          // 3%/s: 自动插枪
                execAction(c.id(), QStringLiteral("plug"), QString(), QStringLiteral("auto"));
                a.startInS = 2;
            }
            break;
        case Charger::Reserved:
            if (a.startInS <= 0 && rng.bounded(100) < 10)
                execAction(c.id(), QStringLiteral("start"), QString(), QStringLiteral("auto"));
            break;
        case Charger::Charging:
            if (rng.bounded(1000) < 5) {                // 0.5%/s: 随机故障(自动恢复)
                execAction(c.id(), QStringLiteral("fault"),
                           QString::number(1 + rng.bounded(3)), QStringLiteral("auto"));
                a.recoverInS = 8 + rng.bounded(8);
            }
            break;
        case Charger::Fault:
            if (a.recoverInS > 0 && --a.recoverInS == 0)
                execAction(c.id(), QStringLiteral("recover"), QString(), QStringLiteral("auto"));
            break;
        case Charger::Offline:
            if (a.onlineInS > 0 && --a.onlineInS == 0)
                execAction(c.id(), QStringLiteral("online"), QString(), QStringLiteral("auto"));
            break;
        case Charger::Finished:
        default:
            break;                                      // 充满自动拔枪由设备自身完成
        }

        // 随机掉线(0.2%/s), 一段时间后自动上线
        if ((c.state() == Charger::Idle || c.state() == Charger::Charging)
                && a.onlineInS == 0 && rng.bounded(1000) < 2) {
            execAction(c.id(), QStringLiteral("offline"), QString(), QStringLiteral("auto"));
            a.onlineInS = 10 + rng.bounded(10);
            a.startInS  = 0;
        }
    }
}

// ------------------------------------------------------------
// 控制台命令与统一动作入口
// ------------------------------------------------------------
void Simulator::drainCommands()
{
    while (true) {
        std::string line;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_cmdQueue.empty()) break;
            line = m_cmdQueue.front();
            m_cmdQueue.pop();
        }
        handleCommand(QString::fromStdString(line));
    }
}

Charger *Simulator::findDevice(int id)
{
    for (Charger &c : m_devices)
        if (c.id() == id) return &c;
    return nullptr;
}

// 统一执行设备动作: 来源 console/server/auto, 成功后打印对应前缀的应答
bool Simulator::execAction(int id, const QString &action, const QString &arg,
                           const QString &source, QString *errOut)
{
    Charger *c = findDevice(id);
    QString err;
    bool ok = false;

    if (!c) {
        err = QStringLiteral("unknown device id %1").arg(id);
    } else if (action == QStringLiteral("plug")) {
        ok = c->plug(&err);
    } else if (action == QStringLiteral("start")) {
        ok = c->start(&err);
    } else if (action == QStringLiteral("stop")) {
        ok = c->stop(&err);
    } else if (action == QStringLiteral("unplug")) {
        ok = c->unplug(&err);
    } else if (action == QStringLiteral("recover")) {
        ok = c->recover(&err);
    } else if (action == QStringLiteral("offline")) {
        ok = c->offline(&err);
    } else if (action == QStringLiteral("online")) {
        ok = c->online(&err);
    } else if (action == QStringLiteral("restart")) {
        ok = c->restart(&err);
    } else if (action == QStringLiteral("fault")) {
        int code = arg.trimmed().toInt();
        ok = c->fault(code <= 0 ? 1 : code, &err);
    } else {
        err = QStringLiteral("unknown action '%1'").arg(action);
    }

    const QString prefix = (source == QStringLiteral("server"))
                               ? QStringLiteral("[ACK] (from server) ")
                               : (source == QStringLiteral("auto")
                                      ? QStringLiteral("[AUTO] ")
                                      : QStringLiteral("[ACK] "));
    if (ok) {
        out() << prefix << "command=" << action << " device=" << id << " ok" << Qt::endl;
    } else {
        out() << prefix << "command=" << action << " device=" << id
              << " fail: " << err << Qt::endl;
    }
    if (errOut)
        *errOut = ok ? QStringLiteral("ok") : err;
    return ok;
}

void Simulator::handleCommand(const QString &line)
{
    const QStringList p = line.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (p.isEmpty()) return;
    const QString cmd = p.at(0).toLower();

    if (cmd == QStringLiteral("help") || cmd == QStringLiteral("h")) {
        out() << "commands:"
                 " list | db | plug <id> | start <id> | stop <id> | unplug <id> |"
                 " fault <id> [code] | recover <id> | offline <id> | online <id> |"
                 " restart <id> | exit"
              << Qt::endl;
    } else if (cmd == QStringLiteral("list") || cmd == QStringLiteral("ls")
               || cmd == QStringLiteral("status")) {
        printStatusTable();
    } else if (cmd == QStringLiteral("db")) {
        printDbInfo();
    } else if (cmd == QStringLiteral("exit") || cmd == QStringLiteral("quit")) {
        m_quitRequested = true;
    } else if (p.size() >= 2) {
        bool okId = false;
        const int id = p.at(1).toInt(&okId);
        if (!okId || !findDevice(id)) {
            out() << "[ACK] command=" << cmd << " fail: unknown device id '"
                  << p.at(1) << "' (list 查看设备)" << Qt::endl;
            return;
        }
        execAction(id, cmd, p.size() >= 3 ? p.at(2) : QString(),
                   QStringLiteral("console"));
    } else {
        out() << "[ACK] unknown command: " << line << " (type 'help')" << Qt::endl;
    }
}

// ------------------------------------------------------------
// 打印
// ------------------------------------------------------------
void Simulator::printStatusTable()
{
    out() << "id    code      state      soc%    power_kW  temp_C  fault  uptime_s"
          << Qt::endl;
    for (Charger &c : m_devices) {
        out() << c.id() << "     " << c.code() << "  "
              << QStringLiteral("%1").arg(c.stateName(), -9)
              << QStringLiteral("%1").arg(round1(c.soc()), 6, 'f', 1)
              << QStringLiteral("%1").arg(round1(c.powerKw()), 9, 'f', 1)
              << QStringLiteral("%1").arg(round1(c.temperatureC()), 8, 'f', 1)
              << QStringLiteral("%1").arg(c.faultCode(), 6)
              << QStringLiteral("%1").arg(c.uptimeS(), 9)
              << Qt::endl;
    }
}

void Simulator::printDbInfo()
{
    out() << "db path   : " << m_db.dbPath() << Qt::endl;
    out() << "frames    : telemetry=" << m_db.telemetryFrames()
          << " heartbeat=" << m_db.heartbeatFrames() << Qt::endl;
    out() << "tables    : charger_telemetry / charger_heartbeat / device_command"
          << " (平台经数据库层读取, 设备不与其他层直连)" << Qt::endl;
}

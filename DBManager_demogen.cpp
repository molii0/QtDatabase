#include "DBManager.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QTime>
#include <QVariant>
#include <QVector>

// ============================================================================
// 演示历史数据生成器(工具, 非业务功能)
// ----------------------------------------------------------------------------
// 用途: 给 Web/图表演示"灌历史"。它不属于业务增删改 —— 业务的增删改一律走
// REST 接口; 本工具只做一件事: 把"过去的历史"批量补进库里(真实世界里那段
// 历史是真实用户一笔一笔用出来的, 演示期没有真实用户, 由本工具代劳)。
//
// 行为:
//   1. 只向前补缺失的更早日期, 已有数据一律不动(幂等, 可重复运行);
//   2. 同一窗口顺手补齐 recharge_log / ops_log / load_prediction
//      (这三张表平时只有业务运行才会积累, 演示库里默认是空的);
//   3. 时间分布与种子一致(周末单量高/早晚高峰/按功率定充电时长), 固定随机
//      种子 + 按"日期"派生种子, 同一天生成的结果与运行顺序无关, 可复现;
//   4. 只插"已完成/已取消"的历史订单, 不碰任何进行中订单与电桩状态。
// ============================================================================

namespace {

// 某一天某分钟的本地时间串
QString tsDay(const QDate &day, int minute)
{
    return QDateTime(QDate(day), QTime(minute / 60, minute % 60))
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// 在"当天能充完"的前提下, 按充电高峰权重随机取一个开始分钟(0..1439)
int pickStartMinute(QRandomGenerator &rng, int durMin)
{
    const int latest = 23 * 60 + 50 - durMin;   // 保证结束不跨到第二天
    if (latest < 0)
        return 0;

    auto weightOf = [](int hour) {
        if (hour >= 17 && hour <= 21) return 8;   // 晚高峰
        if (hour >= 6 && hour <= 9)   return 6;   // 早高峰
        if (hour >= 11 && hour <= 15) return 4;   // 白天
        if (hour == 16 || hour == 22) return 3;
        return 1;                                  // 凌晨低谷
    };
    int totalWeight = 0;
    int cum[24] = {0};
    for (int h = 0; h < 24; ++h) {
        totalWeight += weightOf(h);
        cum[h] = totalWeight;
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        const int roll = rng.bounded(totalWeight);
        int hour = 0;
        for (int h = 0; h < 24; ++h) {
            if (roll < cum[h]) { hour = h; break; }
        }
        const int candidate = hour * 60 + rng.bounded(60);
        if (candidate <= latest)
            return candidate;
    }
    return rng.bounded(latest + 1);   // 兜底
}

// 按功率给充电时长: 120kW 快充 10~55 分, 60kW 快充 15~100 分, 7kW 慢充 30~240 分
int durMinForPower(double power, QRandomGenerator &rng)
{
    if (power >= 100.0) return 10 + rng.bounded(46);
    if (power >= 50.0)  return 15 + rng.bounded(86);
    return 30 + rng.bounded(211);
}

// 日期 -> 确定性随机数(同一天永远同一串随机, 与运行次数/顺序无关)
QRandomGenerator rngForDay(const QDate &day)
{
    const quint32 kBase = 0x9E3779B9u;
    return QRandomGenerator(kBase ^ static_cast<quint32>(day.toJulianDay()));
}

} // namespace

bool DBManager::generateDemoHistory(int days, double density,
                                    DemoGenResult *out, QString *err)
{
    auto fail = [&](const QString &msg) {
        if (err) *err = msg;
        return false;
    };
    if (days < 1 || days > 730)
        return fail(QStringLiteral("天数不合法: 1~730"));
    if (density < 0.1 || density > 20.0)
        return fail(QStringLiteral("密度不合法: 0.1~20"));

    QSqlDatabase dbc = db();

    // ---- 读底数: 主数据(电站/桩/未冻结用户), 没有则无法造历史 ----
    const QVector<Station> stations = listStations();
    if (stations.isEmpty())
        return fail(QStringLiteral("库里没有电站, 无法生成历史(请先用种子初始化)"));

    QHash<qint64, QVector<Charger> > chargersByStation;
    const QVector<Charger> allChargers = listChargers();
    for (const Charger &c : allChargers)
        chargersByStation[c.stationId].append(c);

    QVector<qint64> activeUsers;              // 未冻结用户(历史订单/充值从中选)
    {
        QSqlQuery q(dbc);
        if (!q.exec(QStringLiteral("SELECT user_id FROM user WHERE status = 1 ORDER BY user_id;"))) {
            return fail(QStringLiteral("查询用户失败: %1").arg(q.lastError().text()));
        }
        while (q.next())
            activeUsers.append(q.value(0).toLongLong());
    }
    if (activeUsers.isEmpty())
        return fail(QStringLiteral("库里没有可用用户, 无法生成历史"));

    // ---- 确定"已覆盖到哪天"与"要补的缺失日期" ----
    const QDate today = QDate::currentDate();
    QDate oldestCovered = today;              // 已有历史最早日期(没有则视为今天)
    {
        QSqlQuery q(dbc);
        if (q.exec(QStringLiteral(
                "SELECT date(start_time) FROM charging_order"
                " WHERE start_time IS NOT NULL ORDER BY start_time LIMIT 1;"))
            && q.next()) {
            const QDate d = QDate::fromString(q.value(0).toString(),
                                              QStringLiteral("yyyy-MM-dd"));
            if (d.isValid())
                oldestCovered = d;
        }
    }
    const QDate firstWanted = today.addDays(-days);        // 要求的窗口起点
    // 已有历史比要求窗口更早的日期都算"已覆盖"; 只补 [firstWanted, oldestCovered) 之间的缺失天
    const int missingDays = qMax(0, firstWanted.daysTo(oldestCovered));

    if (out) {
        out->daysRequested = days;
        out->daysGenerated = missingDays;
    }
    if (missingDays == 0)
        return true;                          // 已覆盖, 幂等空跑

    // ---- 订单号接着现有最大值往下排, 保证全局唯一 ----
    qint64 orderSeq = 0;
    {
        QSqlQuery q(dbc);
        if (q.exec(QStringLiteral(
                "SELECT order_no FROM charging_order"
                " ORDER BY LENGTH(order_no) DESC, order_no DESC LIMIT 1;"))
            && q.next()) {
            const QString lastNo = q.value(0).toString();
            bool okNum = false;
            const qint64 n = lastNo.mid(1).toLongLong(&okNum);   // 去掉 T 前缀
            if (okNum)
                orderSeq = n;
        }
    }

    if (!dbc.transaction())
        return fail(QStringLiteral("开启事务失败"));

    QSqlQuery q(dbc);
    q.prepare(QStringLiteral(
        "INSERT INTO charging_order (order_no, user_id, station_id, charger_id, status,"
        "                            energy, amount, start_time, end_time)"
        " VALUES (:no, :uid, :sid, :cid, :st, :en, :am, :sdt, :edt);"));
    QSqlQuery qRecharge(dbc);
    qRecharge.prepare(QStringLiteral(
        "INSERT INTO recharge_log (user_id, amount, created_at)"
        " VALUES (:uid, :am, :ts);"));
    QSqlQuery qOps(dbc);
    qOps.prepare(QStringLiteral(
        "INSERT INTO ops_log (admin_account, charger_id, charger_code, action, detail)"
        " VALUES (:acc, :cid, :cc, :act, :dt);"));
    QSqlQuery qPred(dbc);
    qPred.prepare(QStringLiteral(
        "INSERT INTO load_prediction (station_id, generated_at, target_time, load_kwh,"
        "                             idle_count, is_peak)"
        " VALUES (:sid, :gen, :tgt, :kw, :idle, :peak);"));

    qint64 nOrders = 0, nRecharge = 0, nOps = 0, nPred = 0;

    // 逐日生成: firstWanted .. oldestCovered-1(更早的缺失日期, 从最早到较新, 便于阅读)
    for (QDate day = firstWanted; day < oldestCovered; day = day.addDays(1)) {
        QRandomGenerator rng = rngForDay(day);
        const int dow = day.dayOfWeek();
        const bool weekend = (dow >= 6);
        const QDateTime dayStart(QDate(day), QTime(0, 0));

        // ---------- 历史订单: 每站 3~6 单(周末 3~7), ×密度 ----------
        for (int si = 0; si < stations.size(); ++si) {
            const Station &st = stations.at(si);
            const QVector<Charger> piles = chargersByStation.value(st.stationId);
            if (piles.isEmpty())
                continue;
            const int base = 3 + rng.bounded(weekend ? 5 : 4);
            const int count = qMax(1, qRound(base * density));

            for (int k = 0; k < count; ++k) {
                const qint64 userId = activeUsers.at(rng.bounded(activeUsers.size()));
                const Charger &pile = piles.at(rng.bounded(piles.size()));
                const double power = pile.power;
                const int durMin = durMinForPower(power, rng);
                const int startMinute = pickStartMinute(rng, durMin);
                const QDateTime startTs = dayStart.addSecs(startMinute * 60);
                const bool canceled = rng.generateDouble() < 0.05;

                double energy = 0.0, amount = 0.0;
                QString endTs;
                int status = OrderFinished;
                if (canceled) {
                    status = OrderCanceled;
                } else {
                    energy = round2(power * durMin / 60.0);
                    // 分时电价: 峰/平/谷三段价按充电分钟切段加权(与结算一致, 见 DBManager_price.cpp)
                    const double peak = st.pricePeak > 0.0 ? st.pricePeak : round2(st.price * 1.35);
                    const double valley = st.priceValley > 0.0 ? st.priceValley : round2(st.price * 0.55);
                    const double avgPrice = avgPriceOf(peak, st.price, valley, startTs,
                                                       static_cast<qint64>(durMin) * 60);
                    amount = round2(energy * avgPrice);
                    endTs = startTs.addSecs(durMin * 60)
                                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                }

                q.bindValue(QStringLiteral(":no"),
                            QStringLiteral("T%1").arg(++orderSeq, 5, 10, QLatin1Char('0')));
                q.bindValue(QStringLiteral(":uid"), userId);
                q.bindValue(QStringLiteral(":sid"), st.stationId);
                q.bindValue(QStringLiteral(":cid"), pile.chargerId);
                q.bindValue(QStringLiteral(":st"), status);
                q.bindValue(QStringLiteral(":en"), energy);
                q.bindValue(QStringLiteral(":am"), amount);
                q.bindValue(QStringLiteral(":sdt"), canceled ? QVariant()
                                                            : QVariant(startTs.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
                q.bindValue(QStringLiteral(":edt"), canceled ? QVariant() : QVariant(endTs));
                if (!q.exec()) {
                    dbc.rollback();
                    return fail(QStringLiteral("插入历史订单失败: %1").arg(q.lastError().text()));
                }
                ++nOrders;
            }
        }

        // ---------- 充值流水: 约活跃用户 4%/天, ×密度 ----------
        const int reCount = qMax(0, qRound((activeUsers.size() * 0.04
                                            + rng.bounded(3)) * density));
        for (int k = 0; k < reCount; ++k) {
            qRecharge.bindValue(QStringLiteral(":uid"),
                                activeUsers.at(rng.bounded(activeUsers.size())));
            qRecharge.bindValue(QStringLiteral(":am"),
                                double(20 + rng.bounded(281)));   // 20~300 元
            qRecharge.bindValue(QStringLiteral(":ts"),
                                tsDay(day, rng.bounded(14 * 60))); // 白天居多, 纯演示
            if (!qRecharge.exec()) {
                dbc.rollback();
                return fail(QStringLiteral("插入充值流水失败: %1")
                                .arg(qRecharge.lastError().text()));
            }
            ++nRecharge;
        }

        // ---------- 运维日志: 每天 1~4 条(故障上报/恢复/远程重启), ×密度 ----------
        const int opsCount = qMax(0, qRound((1 + rng.bounded(4)) * density));
        const QStringList actions = {
            QStringLiteral("设备故障上报"),
            QStringLiteral("设备故障恢复"),
            QStringLiteral("远程重启"),
            QStringLiteral("巡检完成"),
        };
        for (int k = 0; k < opsCount; ++k) {
            const Station &st = stations.at(rng.bounded(stations.size()));
            const QVector<Charger> piles = chargersByStation.value(st.stationId);
            if (piles.isEmpty())
                continue;
            const Charger &pile = piles.at(rng.bounded(piles.size()));
            const QString action = actions.at(rng.bounded(actions.size()));
            QString detail;
            if (action == QStringLiteral("设备故障上报"))
                detail = QStringLiteral("故障码 %1").arg(1 + rng.bounded(9));
            else if (action == QStringLiteral("设备故障恢复"))
                detail = QStringLiteral("%1 恢复正常").arg(pile.code);
            else if (action == QStringLiteral("远程重启"))
                detail = QStringLiteral("%1 重启完成").arg(pile.code);
            else
                detail = QStringLiteral("%1 巡检无异常").arg(pile.code);
            qOps.bindValue(QStringLiteral(":acc"), QStringLiteral("system"));
            qOps.bindValue(QStringLiteral(":cid"), pile.chargerId);
            qOps.bindValue(QStringLiteral(":cc"), pile.code);
            qOps.bindValue(QStringLiteral(":act"), action);
            qOps.bindValue(QStringLiteral(":dt"), detail);
            if (!qOps.exec()) {
                dbc.rollback();
                return fail(QStringLiteral("插入运维日志失败: %1").arg(qOps.lastError().text()));
            }
            ++nOps;
        }

        // ---------- 负荷预测: 每站每天 6 个时段(0/4/8/12/16/20 点), 仅供演示曲线 ----------
        const int hourSlots[6] = {0, 4, 8, 12, 16, 20};
        const double utilBase[6] = {0.06, 0.05, 0.20, 0.28, 0.55, 0.48};  // 时段利用率基准
        const double weekendBoost = weekend ? 1.25 : 1.0;
        for (int si = 0; si < stations.size(); ++si) {
            const Station &st = stations.at(si);
            const QVector<Charger> piles = chargersByStation.value(st.stationId);
            if (piles.isEmpty())
                continue;
            double totalKw = 0.0;
            for (const Charger &p : piles)
                totalKw += p.power;
            // generated_at: 前一天晚间 21 点前"完成预测", target 是当天各时段
            const QString generatedAt = dayStart.addSecs(-(3 * 60 * 60))
                                            .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            for (int s = 0; s < 6; ++s) {
                const double util = utilBase[s] * (0.8 + rng.generateDouble() * 0.4)
                                    * weekendBoost;
                const double load = round2(totalKw * 4.0 * util);   // 每时段 4 小时
                qPred.bindValue(QStringLiteral(":sid"), st.stationId);
                qPred.bindValue(QStringLiteral(":gen"), generatedAt);
                qPred.bindValue(QStringLiteral(":tgt"),
                                tsDay(day, hourSlots[s] * 60));
                qPred.bindValue(QStringLiteral(":kw"), load);
                qPred.bindValue(QStringLiteral(":idle"),
                                rng.bounded(static_cast<int>(piles.size()) + 1));
                qPred.bindValue(QStringLiteral(":peak"), (hourSlots[s] == 16 || hourSlots[s] == 20) ? 1 : 0);
                if (!qPred.exec()) {
                    dbc.rollback();
                    return fail(QStringLiteral("插入负荷预测失败: %1").arg(qPred.lastError().text()));
                }
                ++nPred;
            }
        }
    }

    if (!dbc.commit()) {
        dbc.rollback();
        return fail(QStringLiteral("提交事务失败"));
    }

    if (out) {
        out->ordersAdded = nOrders;
        out->rechargesAdded = nRecharge;
        out->opsLogsAdded = nOps;
        out->predictionsAdded = nPred;
    }
    qDebug() << "历史数据生成完成: 补" << missingDays << "天"
             << "(订单 +" << nOrders << ", 充值 +" << nRecharge
             << ", 日志 +" << nOps << ", 预测 +" << nPred << ")";
    return true;
}

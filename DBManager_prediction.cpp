#include "DBManager.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QRandomGenerator>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QTime>
#include <QVector>

// ============================================================================
// 智能预测(load_prediction 表)
// ----------------------------------------------------------------------------
//   读: listPredictions()  —— 管理端"智能预测"页用: 未来 hours 小时的逐时预测
//       曲线 + 各站预测电量(都按窗口 target_time 聚合)
//   写: ensureFuturePredictions() —— 演示/离线工具: 未来 hours 小时的预测
//       缺哪段补哪段(幂等)。真实系统里这张表由 ML 预测脚本写入, 这里给演示库打底。
//
// 说明: 预测窗口从"当前整点"开始(预测点是整点), 用文本时间比较(yyyy-MM-dd HH:mm:ss)。
// ============================================================================

namespace {

constexpr int kMaxPredictHours = 168;    // 最多看未来 7 天

QString toTs(const QDateTime &dt)
{
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

// 预测窗口起点: 当前整点(预测按整点排布)
QDateTime windowStart()
{
    const QTime now = QTime::currentTime();
    return QDateTime(QDate::currentDate(), QTime(now.hour(), 0));
}

int clampHours(int hours)
{
    if (hours <= 0) return 24;
    return hours > kMaxPredictHours ? kMaxPredictHours : hours;
}

// 该时段的预测利用率基准(峰高、谷低, 与分时电价时段一致)
double utilBaseOfHour(int hour)
{
    switch (DBManager::priceBandOfMinute(hour * 60)) {
    case DBManager::BandPeak:   return 0.62;
    case DBManager::BandValley: return 0.16;
    case DBManager::BandFlat:   break;
    }
    return 0.34;
}

} // namespace

// ------------------------- 读: 未来 hours 小时预测 -------------------------

bool DBManager::listPredictions(int hours, qint64 stationId,
                                QVector<PredictionPoint> *curve,
                                QVector<PredictionByStation> *byStation,
                                QString *lastGenerated, QString *err) const
{
    if (!curve || !byStation) {
        if (err) *err = QStringLiteral("listPredictions: 输出参数为空");
        return false;
    }
    curve->clear();
    byStation->clear();
    if (lastGenerated)
        lastGenerated->clear();

    hours = clampHours(hours);
    const bool oneStation = (stationId > 0);
    const QDateTime start = windowStart();
    const QString fromS = toTs(start);
    const QString toS = toTs(start.addSecs(static_cast<qint64>(hours) * 3600));

    // 1) 逐时曲线: 按 target_time 聚合(全部电站相加 / 或只看一个站)
    {
        QString sql = QStringLiteral(
            "SELECT target_time, ROUND(SUM(load_kwh), 2), SUM(idle_count),"
            "       MAX(is_peak), COUNT(DISTINCT station_id)"
            "  FROM load_prediction"
            " WHERE target_time >= :f AND target_time < :t");
        if (oneStation)
            sql += QStringLiteral(" AND station_id = :sid");
        sql += QStringLiteral(" GROUP BY target_time ORDER BY target_time;");

        QSqlQuery q(db());
        q.prepare(sql);
        q.bindValue(QStringLiteral(":f"), fromS);
        q.bindValue(QStringLiteral(":t"), toS);
        if (oneStation)
            q.bindValue(QStringLiteral(":sid"), stationId);
        if (!q.exec()) {
            if (err) *err = QStringLiteral("查询预测曲线失败: %1").arg(q.lastError().text());
            return false;
        }
        while (q.next()) {
            PredictionPoint p;
            p.targetTime = q.value(0).toString();
            p.loadKwh = q.value(1).toDouble();
            p.idleCount = q.value(2).toLongLong();
            p.isPeak = q.value(3).toInt();
            p.stations = q.value(4).toLongLong();
            curve->append(p);
        }
    }

    // 2) 各站未来窗口的预测电量(降序, 供"各站预测电量"排行/柱状图)
    {
        QString sql = QStringLiteral(
            "SELECT p.station_id, s.name, ROUND(SUM(p.load_kwh), 2), COUNT(*),"
            "       ROUND(AVG(p.idle_count), 1)"
            "  FROM load_prediction p"
            "  JOIN station s ON s.station_id = p.station_id"
            " WHERE p.target_time >= :f AND p.target_time < :t");
        if (oneStation)
            sql += QStringLiteral(" AND p.station_id = :sid");
        sql += QStringLiteral(" GROUP BY p.station_id, s.name ORDER BY 3 DESC;");

        QSqlQuery q(db());
        q.prepare(sql);
        q.bindValue(QStringLiteral(":f"), fromS);
        q.bindValue(QStringLiteral(":t"), toS);
        if (oneStation)
            q.bindValue(QStringLiteral(":sid"), stationId);
        if (!q.exec()) {
            if (err) *err = QStringLiteral("查询各站预测电量失败: %1").arg(q.lastError().text());
            return false;
        }
        while (q.next()) {
            PredictionByStation s;
            s.stationId = q.value(0).toLongLong();
            s.name = q.value(1).toString();
            s.loadKwh = q.value(2).toDouble();
            s.points = q.value(3).toLongLong();
            s.avgIdle = q.value(4).toDouble();
            byStation->append(s);
        }
    }

    // 3) 最近一次预测的生成时间(界面显示"预测更新于 ...")
    if (lastGenerated) {
        QString sql = QStringLiteral(
            "SELECT MAX(generated_at) FROM load_prediction"
            " WHERE target_time >= :f AND target_time < :t");
        if (oneStation)
            sql += QStringLiteral(" AND station_id = :sid");
        QSqlQuery q(db());
        q.prepare(sql);
        q.bindValue(QStringLiteral(":f"), fromS);
        q.bindValue(QStringLiteral(":t"), toS);
        if (oneStation)
            q.bindValue(QStringLiteral(":sid"), stationId);
        if (q.exec() && q.next() && !q.value(0).isNull())
            *lastGenerated = q.value(0).toString();
    }
    return true;
}

// ------------------- 写(工具): 补齐未来 hours 小时预测 -------------------

bool DBManager::ensureFuturePredictions(int hours, qint64 *insertedOut, QString *err)
{
    hours = clampHours(hours);
    if (insertedOut)
        *insertedOut = 0;

    QSqlDatabase dbc = db();
    if (!dbc.isOpen()) {
        if (err) *err = QStringLiteral("数据库未打开");
        return false;
    }

    // 需要的底数: 电站 + 该站的桩(总功率/桩数决定预测电量量级)
    const QVector<Station> stations = listStations();
    if (stations.isEmpty()) {
        if (err) *err = QStringLiteral("库里没有电站, 无法生成预测");
        return false;
    }
    QHash<qint64, QVector<Charger> > pilesOf;
    const QVector<Charger> allChargers = listChargers();
    for (const Charger &c : allChargers)
        pilesOf[c.stationId].append(c);

    const QDateTime start = windowStart();
    const QString fromS = toTs(start);
    const QString toS = toTs(start.addSecs(static_cast<qint64>(hours) * 3600));

    // 已有 (电站, 时刻) 组合, 保证"缺哪段补哪段"
    QSet<QString> existing;
    {
        QSqlQuery q(dbc);
        q.prepare(QStringLiteral(
            "SELECT station_id, target_time FROM load_prediction"
            " WHERE target_time >= :f AND target_time < :t;"));
        q.bindValue(QStringLiteral(":f"), fromS);
        q.bindValue(QStringLiteral(":t"), toS);
        if (!q.exec()) {
            if (err) *err = QStringLiteral("查询已有预测失败: %1").arg(q.lastError().text());
            return false;
        }
        while (q.next())
            existing.insert(QStringLiteral("%1|%2")
                                .arg(q.value(0).toLongLong())
                                .arg(q.value(1).toString()));
    }

    QSqlQuery ins(dbc);
    ins.prepare(QStringLiteral(
        "INSERT INTO load_prediction (station_id, generated_at, target_time, load_kwh,"
        "                             idle_count, is_peak)"
        " VALUES (:sid, :gen, :tgt, :kw, :idle, :peak);"));

    qint64 inserted = 0;
    // 生成时间统一记为"本次预测运行时间(当前整点)": 真实系统里是 ML 脚本的运行时间,
    // 前端用 generatedAt 显示"预测更新于 ..."
    const QString genS = fromS;
    for (int h = 0; h < hours; ++h) {
        const QDateTime target = start.addSecs(static_cast<qint64>(h) * 3600);
        const QString targetS = toTs(target);
        const int isPeak = priceBandOfMinute(target.time().hour() * 60) == BandPeak ? 1 : 0;
        const double util = utilBaseOfHour(target.time().hour());

        for (const Station &st : stations) {
            const QVector<Charger> piles = pilesOf.value(st.stationId);
            if (piles.isEmpty())
                continue;
            if (existing.contains(QStringLiteral("%1|%2").arg(st.stationId).arg(targetS)))
                continue;

            double totalKw = 0.0;
            for (const Charger &p : piles)
                totalKw += p.power;

            // 每个(站,时段)用固定随机种子: 同一时段重复生成的结果一致
            QRandomGenerator rng(static_cast<quint32>(target.toSecsSinceEpoch())
                                 ^ static_cast<quint32>(st.stationId * 2654435761u));
            const double load = round2(totalKw * util * (0.85 + rng.generateDouble() * 0.30));

            ins.bindValue(QStringLiteral(":sid"), st.stationId);
            ins.bindValue(QStringLiteral(":gen"), genS);
            ins.bindValue(QStringLiteral(":tgt"), targetS);
            ins.bindValue(QStringLiteral(":kw"), load);
            ins.bindValue(QStringLiteral(":idle"), rng.bounded(piles.size() + 1));
            ins.bindValue(QStringLiteral(":peak"), isPeak);
            if (!ins.exec()) {
                if (err) *err = QStringLiteral("写入预测失败: %1").arg(ins.lastError().text());
                return false;
            }
            ++inserted;
        }
    }

    if (insertedOut)
        *insertedOut = inserted;
    if (inserted > 0)
        qDebug() << "未来预测补充:" << hours << "小时窗口, 新增" << inserted << "行";
    return true;
}

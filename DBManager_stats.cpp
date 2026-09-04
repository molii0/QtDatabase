#include "DBManager.h"

#include <QDate>
#include <QDebug>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>

// ============================================================================
// 统计(管理端销售业绩 / 电桩状态总览用)
//   只统计"已完成"(status=2) 的订单; 金额按 end_time 所属日期归属。
// ============================================================================

bool DBManager::chargerStatusCount(ChargerStatusCount *out, QString *err) const
{
    if (!out) {
        if (err) *err = QStringLiteral("chargerStatusCount: 输出参数为空");
        return false;
    }
    *out = ChargerStatusCount();
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("SELECT status, COUNT(*) FROM charger GROUP BY status;"))) {
        if (err) *err = QStringLiteral("统计电桩状态失败: %1").arg(q.lastError().text());
        return false;
    }
    while (q.next()) {
        const int st = q.value(0).toInt();
        const qint64 n = q.value(1).toLongLong();
        if (st == ChargerIdle)           out->idle += n;
        else if (st == ChargerConnected) out->connected += n;
        else if (st == ChargerCharging)  out->charging += n;
        else if (st == ChargerFault)     out->fault += n;
    }
    out->total = out->idle + out->connected + out->charging + out->fault;
    return true;
}

bool DBManager::revenueSummary(RevenueSummary *out, QString *err) const
{
    if (!out) {
        if (err) *err = QStringLiteral("revenueSummary: 输出参数为空");
        return false;
    }
    *out = RevenueSummary();
    QSqlQuery q(db());
    // 今日
    if (!q.exec(QStringLiteral(
            "SELECT COALESCE(ROUND(SUM(amount), 2), 0) FROM charging_order"
            " WHERE status = 2 AND date(end_time) = date('now', 'localtime');"))
        || !q.next()) {
        if (err) *err = QStringLiteral("统计今日营收失败: %1").arg(q.lastError().text());
        return false;
    }
    out->today = q.value(0).toDouble();
    // 本月
    if (!q.exec(QStringLiteral(
            "SELECT COALESCE(ROUND(SUM(amount), 2), 0) FROM charging_order"
            " WHERE status = 2 AND strftime('%Y-%m', end_time) = strftime('%Y-%m', 'now', 'localtime');"))
        || !q.next()) {
        if (err) *err = QStringLiteral("统计本月营收失败: %1").arg(q.lastError().text());
        return false;
    }
    out->month = q.value(0).toDouble();
    // 总营收
    if (!q.exec(QStringLiteral(
            "SELECT COALESCE(ROUND(SUM(amount), 2), 0) FROM charging_order WHERE status = 2;"))
        || !q.next()) {
        if (err) *err = QStringLiteral("统计总营收失败: %1").arg(q.lastError().text());
        return false;
    }
    out->total = q.value(0).toDouble();
    return true;
}

bool DBManager::dailyRevenue(int days, QVector<RevenueDay> *out, QString *err) const
{
    if (!out || days <= 0 || days > 366) {
        if (err) *err = QStringLiteral("dailyRevenue: 参数非法(days 需在 1~366)");
        return false;
    }
    out->clear();

    // 先按日期聚合(只统计已完成订单)
    QHash<QString, RevenueDay> byDay;
    {
        QSqlQuery q(db());
        q.prepare(QStringLiteral(
            "SELECT date(end_time) AS d, ROUND(SUM(amount), 2), COUNT(*) FROM charging_order"
            " WHERE status = 2 AND end_time >= :start AND end_time < :end GROUP BY d;"));
        const QString startStr = QDate::currentDate().addDays(-(days - 1))
                                     .toString(QStringLiteral("yyyy-MM-dd"))
                                 + QStringLiteral(" 00:00:00");
        const QString endStr = QDate::currentDate().addDays(1)
                                   .toString(QStringLiteral("yyyy-MM-dd"))
                               + QStringLiteral(" 00:00:00");
        q.bindValue(QStringLiteral(":start"), startStr);
        q.bindValue(QStringLiteral(":end"), endStr);
        if (!q.exec()) {
            if (err) *err = QStringLiteral("按日统计营收失败: %1").arg(q.lastError().text());
            return false;
        }
        while (q.next()) {
            RevenueDay r;
            r.day = q.value(0).toString();
            r.amount = q.value(1).toDouble();
            r.orders = q.value(2).toLongLong();
            byDay.insert(r.day, r);
        }
    }
    // 从最早一天到今天逐天输出, 没有订单的日子补 0
    for (int i = days - 1; i >= 0; --i) {
        const QString day = QDate::currentDate().addDays(-i)
                                .toString(QStringLiteral("yyyy-MM-dd"));
        if (byDay.contains(day))
            out->append(byDay.value(day));
        else
            out->append(RevenueDay{day, 0.0, 0});
    }
    return true;
}

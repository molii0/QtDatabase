#include "DBManager.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

// ============================================================================
// 充电站(station) 与 充电桩(charger) 的数据操作
// 含: 新增/修改/删除电站、新增/删除电桩、批量建桩自动编号(UC-A-05/06)
// ============================================================================

// ------------------------- 充电站 -------------------------
QVector<DBManager::Station> DBManager::listStations() const
{
    QVector<Station> result;
    QSqlQuery q(db());
    q.exec(QStringLiteral("SELECT station_id, name, code_prefix, address, longitude, latitude, price "
                          "FROM station ORDER BY station_id;"));
    while (q.next()) {
        Station s;
        s.stationId = q.value(0).toLongLong();
        s.name = q.value(1).toString();
        s.codePrefix = q.value(2).toString();
        s.address = q.value(3).toString();
        s.longitude = q.value(4).toDouble();
        s.latitude = q.value(5).toDouble();
        s.price = q.value(6).toDouble();
        result.append(s);
    }
    return result;
}

bool DBManager::getStation(qint64 stationId, Station *out) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT station_id, name, code_prefix, address, longitude, latitude, price "
                             "FROM station WHERE station_id = :id;"));
    q.bindValue(QStringLiteral(":id"), stationId);
    if (!q.exec() || !q.next())
        return false;
    if (out) {
        out->stationId = q.value(0).toLongLong();
        out->name = q.value(1).toString();
        out->codePrefix = q.value(2).toString();
        out->address = q.value(3).toString();
        out->longitude = q.value(4).toDouble();
        out->latitude = q.value(5).toDouble();
        out->price = q.value(6).toDouble();
    }
    return true;
}

bool DBManager::addStation(const QString &name, const QString &codePrefix,
                           const QString &address, double longitude, double latitude,
                           double price, qint64 *newStationId, QString *err)
{
    if (name.trimmed().isEmpty()) {
        if (err) *err = QStringLiteral("充电站名称不能为空");
        return false;
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO station (name, code_prefix, address, longitude, latitude, price) "
        "VALUES (:n, :cp, :ad, :lng, :lat, :p);"));
    q.bindValue(QStringLiteral(":n"), name.trimmed());
    q.bindValue(QStringLiteral(":cp"), codePrefix.trimmed());
    q.bindValue(QStringLiteral(":ad"), address);
    q.bindValue(QStringLiteral(":lng"), longitude);
    q.bindValue(QStringLiteral(":lat"), latitude);
    q.bindValue(QStringLiteral(":p"), price);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("新增充电站失败: %1").arg(q.lastError().text());
        return false;
    }
    if (newStationId)
        *newStationId = q.lastInsertId().toLongLong();
    return true;
}

bool DBManager::updateStation(qint64 stationId, const QString &name, const QString &codePrefix,
                              const QString &address, double longitude, double latitude,
                              double price, QString *err)
{
    if (name.trimmed().isEmpty()) {
        if (err) *err = QStringLiteral("充电站名称不能为空");
        return false;
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "UPDATE station SET name = :n, code_prefix = :cp, address = :ad,"
        " longitude = :lng, latitude = :lat, price = :p WHERE station_id = :id;"));
    q.bindValue(QStringLiteral(":n"), name.trimmed());
    q.bindValue(QStringLiteral(":cp"), codePrefix.trimmed());
    q.bindValue(QStringLiteral(":ad"), address);
    q.bindValue(QStringLiteral(":lng"), longitude);
    q.bindValue(QStringLiteral(":lat"), latitude);
    q.bindValue(QStringLiteral(":p"), price);
    q.bindValue(QStringLiteral(":id"), stationId);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("修改充电站失败: %1").arg(q.lastError().text());
        return false;
    }
    if (q.numRowsAffected() == 0) {
        if (err) *err = QStringLiteral("充电站不存在 (station_id=%1)").arg(stationId);
        return false;
    }
    return true;
}

bool DBManager::deleteStation(qint64 stationId, QString *err)
{
    // BR-10: 电站下仍有电桩时禁止删除
    bool has = false;
    stationHasChargers(stationId, &has);
    if (has) {
        if (err) *err = QStringLiteral("该充电站下仍有电桩，禁止删除");
        return false;
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM station WHERE station_id = :id;"));
    q.bindValue(QStringLiteral(":id"), stationId);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("删除充电站失败: %1").arg(q.lastError().text());
        return false;
    }
    if (q.numRowsAffected() == 0) {
        if (err) *err = QStringLiteral("充电站不存在 (station_id=%1)").arg(stationId);
        return false;
    }
    return true;
}

bool DBManager::stationHasChargers(qint64 stationId, bool *has) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM charger WHERE station_id = :id;"));
    q.bindValue(QStringLiteral(":id"), stationId);
    if (!q.exec() || !q.next())
        return false;
    if (has)
        *has = q.value(0).toLongLong() > 0;
    return true;
}

// ------------------------- 充电桩 -------------------------

QVector<DBManager::Charger> DBManager::listChargers(qint64 stationId) const
{
    QVector<Charger> result;
    QSqlQuery q(db());
    if (stationId < 0) {
        q.exec(QStringLiteral("SELECT charger_id, station_id, code, type, power, status "
                              "FROM charger ORDER BY station_id, code;"));
    } else {
        q.prepare(QStringLiteral("SELECT charger_id, station_id, code, type, power, status "
                                 "FROM charger WHERE station_id = :id ORDER BY code;"));
        q.bindValue(QStringLiteral(":id"), stationId);
        q.exec();
    }
    while (q.next()) {
        Charger c;
        c.chargerId = q.value(0).toLongLong();
        c.stationId = q.value(1).toLongLong();
        c.code = q.value(2).toString();
        c.type = q.value(3).toInt();
        c.power = q.value(4).toDouble();
        c.status = q.value(5).toInt();
        result.append(c);
    }
    return result;
}

bool DBManager::getCharger(qint64 chargerId, Charger *out) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT charger_id, station_id, code, type, power, status "
                             "FROM charger WHERE charger_id = :id;"));
    q.bindValue(QStringLiteral(":id"), chargerId);
    if (!q.exec() || !q.next())
        return false;
    if (out) {
        out->chargerId = q.value(0).toLongLong();
        out->stationId = q.value(1).toLongLong();
        out->code = q.value(2).toString();
        out->type = q.value(3).toInt();
        out->power = q.value(4).toDouble();
        out->status = q.value(5).toInt();
    }
    return true;
}

bool DBManager::addCharger(qint64 stationId, const QString &code, int type, double power,
                           QString *err)
{
    Station st;
    if (!getStation(stationId, &st)) {
        if (err) *err = QStringLiteral("电站不存在 (station_id=%1)").arg(stationId);
        return false;
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT INTO charger (station_id, code, type, power, status) "
                             "VALUES (:sid, :code, :type, :pw, 0);"));
    q.bindValue(QStringLiteral(":sid"), stationId);
    q.bindValue(QStringLiteral(":code"), code.trimmed());
    q.bindValue(QStringLiteral(":type"), type);
    q.bindValue(QStringLiteral(":pw"), power);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("新增电桩失败(站内编号可能重复): %1").arg(q.lastError().text());
        return false;
    }
    return true;
}

bool DBManager::batchAddChargers(qint64 stationId, int count, int type, double power,
                                 int *created, QString *err)
{
    if (count <= 0 || count > 100) {
        if (err) *err = QStringLiteral("一次批量建桩数量需在 1~100 之间");
        return false;
    }
    Station st;
    if (!getStation(stationId, &st)) {
        if (err) *err = QStringLiteral("电站不存在 (station_id=%1)").arg(stationId);
        return false;
    }
    // 编号前缀: 优先用站点缩写(如 DR); 没填缩写就用 S<电站ID>
    const QString prefix = st.codePrefix.trimmed().isEmpty()
        ? QStringLiteral("S%1").arg(stationId)
        : st.codePrefix.trimmed();

    // 找出该站已有 "前缀-NN" 里的最大序号, 从下一个开始编
    qint64 maxSeq = 0;
    {
        QSqlQuery q(db());
        q.prepare(QStringLiteral("SELECT code FROM charger WHERE station_id = :sid AND code LIKE :pat;"));
        q.bindValue(QStringLiteral(":sid"), stationId);
        q.bindValue(QStringLiteral(":pat"), prefix + QStringLiteral("-%"));
        if (q.exec()) {
            while (q.next()) {
                const QStringList parts = q.value(0).toString().split(QLatin1Char('-'));
                if (parts.size() == 2) {
                    bool ok = false;
                    const qint64 seq = parts.at(1).toLongLong(&ok);
                    if (ok)
                        maxSeq = qMax(maxSeq, seq);
                }
            }
        }
    }

    // 整批在一个事务里, 中途出错整体回滚
    if (!beginTransaction()) {
        if (err) *err = QStringLiteral("批量建桩: 开启事务失败");
        return false;
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT INTO charger (station_id, code, type, power, status) "
                             "VALUES (:sid, :code, :type, :pw, 0);"));
    int done = 0;
    for (int i = 0; i < count; ++i) {
        const QString code = prefix + QStringLiteral("-")
                             + QString::number(++maxSeq).rightJustified(2, QLatin1Char('0'));
        q.bindValue(QStringLiteral(":sid"), stationId);
        q.bindValue(QStringLiteral(":code"), code);
        q.bindValue(QStringLiteral(":type"), type);
        q.bindValue(QStringLiteral(":pw"), power);
        if (!q.exec()) {
            if (err) *err = QStringLiteral("批量建桩第 %1 台失败: %2")
                                .arg(i + 1).arg(q.lastError().text());
            rollbackTransaction();
            return false;
        }
        ++done;
    }
    if (!commitTransaction()) {
        if (err) *err = QStringLiteral("批量建桩: 提交事务失败");
        rollbackTransaction();
        return false;
    }
    if (created)
        *created = done;
    return true;
}

bool DBManager::deleteCharger(qint64 chargerId, QString *err)
{
    Charger c;
    if (!getCharger(chargerId, &c)) {
        if (err) *err = QStringLiteral("电桩不存在 (charger_id=%1)").arg(chargerId);
        return false;
    }
    // 有"进行中"的订单(已连接/充电中)不能删除
    if (c.status == ChargerConnected || c.status == ChargerCharging) {
        if (err) *err = QStringLiteral("电桩 %1 当前为[%2]，禁止删除(请先完成/取消订单)")
                            .arg(c.code, chargerStateText(c.status));
        return false;
    }
    // 已有订单(含历史订单)引用该桩时禁止删除, 避免丢历史
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM charging_order WHERE charger_id = :id;"));
    q.bindValue(QStringLiteral(":id"), chargerId);
    if (q.exec() && q.next() && q.value(0).toLongLong() > 0) {
        if (err) *err = QStringLiteral("电桩 %1 已有订单记录，禁止删除").arg(c.code);
        return false;
    }
    q.prepare(QStringLiteral("DELETE FROM charger WHERE charger_id = :id;"));
    q.bindValue(QStringLiteral(":id"), chargerId);
    if (!q.exec()) {
        if (err) *err = QStringLiteral("删除电桩失败: %1").arg(q.lastError().text());
        return false;
    }
    return true;
}

// 管理员标记故障: 只有"空闲"桩允许直接标记, 避免影响正在进行的订单
bool DBManager::adminMarkFault(qint64 chargerId, QString *err)
{
    Charger c;
    if (!getCharger(chargerId, &c)) {
        if (err) *err = QStringLiteral("电桩不存在");
        return false;
    }
    if (c.status != ChargerIdle) {
        if (err) *err = QStringLiteral("电桩 %1 当前为[%2]，请先处理相关订单再标记故障")
                            .arg(c.code, chargerStateText(c.status));
        return false;
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE charger SET status = 3 WHERE charger_id = :id AND status = 0;"));
    q.bindValue(QStringLiteral(":id"), chargerId);
    if (!q.exec() || q.numRowsAffected() != 1) {
        if (err) *err = QStringLiteral("标记故障失败");
        return false;
    }
    return true;
}

// 管理员远程重启/恢复正常: 故障 -> 空闲
bool DBManager::adminRecover(qint64 chargerId, QString *err)
{
    Charger c;
    if (!getCharger(chargerId, &c)) {
        if (err) *err = QStringLiteral("电桩不存在");
        return false;
    }
    if (c.status != ChargerFault) {
        if (err) *err = QStringLiteral("电桩 %1 当前为[%2]，无需恢复").arg(c.code, chargerStateText(c.status));
        return false;
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE charger SET status = 0 WHERE charger_id = :id AND status = 3;"));
    q.bindValue(QStringLiteral(":id"), chargerId);
    if (!q.exec() || q.numRowsAffected() != 1) {
        if (err) *err = QStringLiteral("恢复电桩失败");
        return false;
    }
    return true;
}

// ------------------------- 运维日志(ops_log) -------------------------

bool DBManager::addOpsLog(const QString &adminAccount, qint64 chargerId,
                          const QString &chargerCode, const QString &action,
                          const QString &detail)
{
    // detail 可省略; 省略时存"非 NULL 的空串"(避免触发表上的 NOT NULL)
    const QString detailText = detail.isNull() ? QStringLiteral("") : detail;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO ops_log (admin_account, charger_id, charger_code, action, detail)"
        " VALUES (:aa, :cid, :cc, :ac, :dt);"));
    q.bindValue(QStringLiteral(":aa"), adminAccount);
    q.bindValue(QStringLiteral(":cid"), chargerId);
    q.bindValue(QStringLiteral(":cc"), chargerCode);
    q.bindValue(QStringLiteral(":ac"), action);
    q.bindValue(QStringLiteral(":dt"), detailText);
    if (!q.exec()) {
        qDebug() << "写入运维日志失败:" << q.lastError().text();
        return false;
    }
    return true;
}

bool DBManager::listOpsLogs(int limit, QVector<OpsLog> *out, QString *err) const
{
    if (!out) {
        if (err) *err = QStringLiteral("listOpsLogs: 输出参数为空");
        return false;
    }
    out->clear();
    QSqlQuery q(db());
    if (limit < 0) {
        q.prepare(QStringLiteral(
            "SELECT log_id, admin_account, charger_id, charger_code, action, detail, created_at"
            " FROM ops_log ORDER BY log_id DESC;"));
    } else {
        q.prepare(QStringLiteral(
            "SELECT log_id, admin_account, charger_id, charger_code, action, detail, created_at"
            " FROM ops_log ORDER BY log_id DESC LIMIT :lim;"));
        q.bindValue(QStringLiteral(":lim"), limit);
    }
    if (!q.exec()) {
        if (err) *err = QStringLiteral("查询运维日志失败: %1").arg(q.lastError().text());
        return false;
    }
    while (q.next()) {
        OpsLog o;
        o.logId = q.value(0).toLongLong();
        o.adminAccount = q.value(1).toString();
        o.chargerId = q.value(2).toLongLong();
        o.chargerCode = q.value(3).toString();
        o.action = q.value(4).toString();
        o.detail = q.value(5).toString();
        o.createdAt = q.value(6).toString();
        out->append(o);
    }
    return true;
}

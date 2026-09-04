#include "DBManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QThreadStorage>

#include <cmath>

// ============================================================================
// DBManager 核心部分: 连接管理 / 建表 / 索引 / 初始化
// (用户、电站桩、订单的实现分别在 DBManager_user/station/order.cpp,
//  演示数据在 DBManager_seed.cpp)
// ============================================================================

namespace {
const QString kDefaultDbName = QStringLiteral("charge_platform.db");
} // namespace

// ------------------------- 单例与生命周期 -------------------------

DBManager &DBManager::instance()
{
    static DBManager mgr;
    return mgr;
}

DBManager::DBManager() = default;

DBManager::~DBManager() = default;

bool DBManager::init(const QString &dbFilePath)
{
    QString path = dbFilePath;
    if (path.isEmpty())
        path = QDir(QCoreApplication::applicationDirPath()).filePath(kDefaultDbName);
    m_dbPath = QDir::cleanPath(path);

    // 确保数据库所在目录存在
    const QDir dir = QFileInfo(m_dbPath).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        qCritical() << "创建数据库目录失败:" << dir.absolutePath();
        return false;
    }

    const QSqlDatabase dbc = db();          // 打开当前线程(一般是主线程)的连接
    if (!dbc.isOpen()) {
        qCritical() << "打开数据库失败:" << m_dbPath;
        return false;
    }

    // 先判断数据库文件是不是"全新的"(里面还没有任何业务表)
    bool freshFile = true;
    {
        QSqlQuery probe(dbc);
        if (probe.exec(QStringLiteral(
                "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%';"))
            && probe.next()) {
            freshFile = false;
        }
    }

    if (!createTables())
        return false;

    if (freshFile) {
        // 全新文件: 上面建的就是最新结构, 直接登记版本号
        if (!writeSchemaVersion(kSchemaVersion)) {
            qCritical() << "登记数据库版本失败";
            return false;
        }
    } else {
        // 已有文件: 读出结构版本号, 把没执行过的增量迁移按顺序执行一遍
        // (迁移必须跑在建索引之前: BR-02/03 唯一索引要求先清理历史重复订单)
        int version = 0;
        readSchemaVersion(&version);
        QString migErr;
        if (!applyMigrations(version, &migErr)) {
            qCritical() << "数据库迁移失败:" << migErr;
            return false;
        }
    }

    if (!createIndexes())
        return false;

    // 空库(没有任何电站)才写演示数据, 已存在的库不重复写
    QSqlQuery q(dbc);
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM station;")) && q.next()
        && q.value(0).toInt() == 0) {
        if (!seedData())
            return false;
        qDebug() << "已写入演示数据到:" << m_dbPath;
    }

    qDebug() << "数据库就绪:" << m_dbPath;
    return true;
}

bool DBManager::isOpen() const
{
    return !m_dbPath.isEmpty() && db().isOpen();
}

QString DBManager::dbPath() const
{
    return m_dbPath;
}

// 线程安全的连接获取: 每个线程第一次调用时建一条自己的连接。
// 说明: QSqlDatabase 不能跨线程使用, 所以各线程各用各的连接;
//       这些连接在线程结束时由 QThreadStorage 自动销毁(进程退出时统一关闭)。
QSqlDatabase DBManager::db() const
{
    if (m_dbPath.isEmpty()) {
        qCritical() << "尚未调用 init() 指定数据库文件";
        return QSqlDatabase();
    }
    static QThreadStorage<QSqlDatabase> tls;

    if (!tls.hasLocalData()) {
        QMutexLocker lock(&m_openMutex);    // addDatabase 会改全局注册表, 需要互斥
        if (!tls.hasLocalData()) {
            // 连接名带线程 ID, 保证各线程互不共用
            const QString connName = QStringLiteral("conn_%1")
                                         .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
            QSqlDatabase c = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
            c.setDatabaseName(m_dbPath);
            if (!c.open()) {
                qCritical() << "线程连接打开失败:" << c.lastError().text();
                return QSqlDatabase();
            }
            // 每个连接都要单独设置(跨进程同时读写的关键配置):
            //   WAL 模式: 读写互不阻塞, 两个客户端可同时访问
            //   外键约束: 开启
            //   busy_timeout: 写锁被占用时最多等 5 秒, 避免立刻报 "database is locked"
            QSqlQuery q(c);
            q.exec(QStringLiteral("PRAGMA journal_mode = WAL;"));
            q.exec(QStringLiteral("PRAGMA foreign_keys = ON;"));
            q.exec(QStringLiteral("PRAGMA busy_timeout = 5000;"));
            tls.setLocalData(c);
        }
    }
    return tls.localData();
}

// ------------------------- 事务 -------------------------

bool DBManager::beginTransaction()   { return db().transaction(); }
bool DBManager::commitTransaction()  { return db().commit(); }
bool DBManager::rollbackTransaction(){ return db().rollback(); }

// ------------------------- 建表与索引 -------------------------

bool DBManager::createTables()
{
    const QStringList sqls = {
        // 结构版本表(数据库维护机制: 记录当前结构版本号, 启动时按版本补迁移)
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS schema_version (
                version    INTEGER PRIMARY KEY,
                applied_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
            );
        )"),
        // 用户表
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS user (
                user_id       INTEGER PRIMARY KEY AUTOINCREMENT,
                phone         TEXT    NOT NULL UNIQUE,
                nickname      TEXT    NOT NULL,
                balance       REAL    NOT NULL DEFAULT 0,
                status        INTEGER NOT NULL DEFAULT 1,
                register_time TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
            );
        )"),
        // 管理员表
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS admin (
                admin_id   INTEGER PRIMARY KEY AUTOINCREMENT,
                account    TEXT NOT NULL UNIQUE,
                password   TEXT NOT NULL,
                created_at TEXT NOT NULL DEFAULT (datetime('now','localtime'))
            );
        )"),
        // 充电站表
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS station (
                station_id  INTEGER PRIMARY KEY AUTOINCREMENT,
                name        TEXT NOT NULL,
                code_prefix TEXT NOT NULL DEFAULT '',
                address     TEXT NOT NULL DEFAULT '',
                longitude   REAL NOT NULL,
                latitude    REAL NOT NULL,
                price       REAL NOT NULL DEFAULT 0
            );
        )"),
        // 充电桩表(status 见 ChargeState.h: 0空闲 1已连接 2充电中 3故障)
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS charger (
                charger_id INTEGER PRIMARY KEY AUTOINCREMENT,
                station_id INTEGER NOT NULL REFERENCES station(station_id),
                code       TEXT    NOT NULL,
                type       INTEGER NOT NULL DEFAULT 0,
                power      REAL    NOT NULL,
                status     INTEGER NOT NULL DEFAULT 0,
                UNIQUE (station_id, code)
            );
        )"),
        // 订单表(status 见 ChargeState.h: 0待支付 1充电中 2已完成 3已取消)
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS charging_order (
                order_id   INTEGER PRIMARY KEY AUTOINCREMENT,
                order_no   TEXT NOT NULL UNIQUE,
                user_id    INTEGER NOT NULL REFERENCES user(user_id),
                station_id INTEGER NOT NULL REFERENCES station(station_id),
                charger_id INTEGER NOT NULL REFERENCES charger(charger_id),
                status     INTEGER NOT NULL DEFAULT 0,
                energy     REAL    NOT NULL DEFAULT 0,
                amount     REAL    NOT NULL DEFAULT 0,
                start_time TEXT,
                end_time   TEXT,
                created_at TEXT    NOT NULL DEFAULT (datetime('now','localtime'))
            );
        )"),
        // 运维日志表(UC-A-05: 远程重启/标记故障/恢复正常等记录)
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS ops_log (
                log_id        INTEGER PRIMARY KEY AUTOINCREMENT,
                admin_account TEXT NOT NULL DEFAULT '',
                charger_id    INTEGER REFERENCES charger(charger_id),
                charger_code  TEXT NOT NULL DEFAULT '',
                action        TEXT NOT NULL,
                detail        TEXT NOT NULL DEFAULT '',
                created_at    TEXT NOT NULL DEFAULT (datetime('now','localtime'))
            );
        )"),
        // 充值流水表(UC-U-05: 每次充值记一条)
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS recharge_log (
                recharge_id INTEGER PRIMARY KEY AUTOINCREMENT,
                user_id     INTEGER NOT NULL REFERENCES user(user_id),
                amount      REAL NOT NULL,
                created_at  TEXT NOT NULL DEFAULT (datetime('now','localtime'))
            );
        )"),
        // 负荷预测表(UC-A-08/UC-M-03: 机器学习结果回写)
        QStringLiteral(R"(
            CREATE TABLE IF NOT EXISTS load_prediction (
                prediction_id INTEGER PRIMARY KEY AUTOINCREMENT,
                station_id    INTEGER NOT NULL REFERENCES station(station_id),
                generated_at  TEXT NOT NULL,
                target_time   TEXT NOT NULL,
                load_kwh      REAL NOT NULL DEFAULT 0,
                idle_count    INTEGER NOT NULL DEFAULT 0,
                is_peak       INTEGER NOT NULL DEFAULT 0
            );
        )"),
    };

    QSqlQuery q(db());
    for (const QString &sql : sqls) {
        if (!q.exec(sql)) {
            qCritical() << "建表失败:" << q.lastError().text() << "\nSQL:" << sql;
            return false;
        }
    }
    return true;
}

bool DBManager::createIndexes()
{
    // 检索加速索引(IF NOT EXISTS: 旧库补建也不会重复)
    const QStringList sqls = {
        // 电站下的电桩列表查询: WHERE station_id = ?
        "CREATE INDEX IF NOT EXISTS idx_charger_station ON charger (station_id);",
        // 电桩按状态过滤(电站列表"空闲数"统计)
        "CREATE INDEX IF NOT EXISTS idx_charger_status ON charger (status);",
        // 某用户的订单历史: WHERE user_id = ?
        "CREATE INDEX IF NOT EXISTS idx_order_user ON charging_order (user_id);",
        // 查"某用户未结算订单"(预约0/充电中1) —— 联合索引
        "CREATE INDEX IF NOT EXISTS idx_order_user_status ON charging_order (user_id, status);",
        // 按订单状态筛选(管理端订单页)
        "CREATE INDEX IF NOT EXISTS idx_order_status ON charging_order (status);",
        // 某电桩是否被订单占用 / 电桩使用记录
        "CREATE INDEX IF NOT EXISTS idx_order_charger ON charging_order (charger_id);",

        // ---- BR-02/BR-03 数据库级约束: 部分唯一索引 ----
        // 同一用户同时最多 1 个未结算订单(待支付0/充电中1)
        "CREATE UNIQUE INDEX IF NOT EXISTS uq_order_user_active"
        " ON charging_order (user_id) WHERE status IN (0, 1);",
        // 同一电桩同时最多被 1 个订单占用
        "CREATE UNIQUE INDEX IF NOT EXISTS uq_order_charger_active"
        " ON charging_order (charger_id) WHERE status IN (0, 1);",

        // 新表配套索引
        "CREATE INDEX IF NOT EXISTS idx_recharge_user ON recharge_log (user_id);",
        "CREATE INDEX IF NOT EXISTS idx_pred_station ON load_prediction (station_id);",
    };

    QSqlQuery q(db());
    for (const QString &sql : sqls) {
        if (!q.exec(sql)) {
            qCritical() << "建索引失败:" << q.lastError().text() << "\nSQL:" << sql;
            return false;
        }
    }
    qDebug() << "索引检查完成(不存在会自动创建)。";
    return true;
}

// ------------------------- 数据库版本与增量迁移 -------------------------

bool DBManager::readSchemaVersion(int *version) const
{
    *version = 0;
    QSqlQuery q(db());
    if (q.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_version;")) && q.next())
        *version = q.value(0).toInt();
    return true;
}

bool DBManager::writeSchemaVersion(int version)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO schema_version (version, applied_at) VALUES (:v, :t);"));
    q.bindValue(QStringLiteral(":v"), version);
    q.bindValue(QStringLiteral(":t"), nowStr());
    if (!q.exec()) {
        qCritical() << "写 schema_version 失败:" << q.lastError().text();
        return false;
    }
    return true;
}

// 按顺序执行尚未应用的增量迁移(每步一个事务, 只做增/改, 不删表不重建)。
// 以后结构要变: 在这里按顺序追加一步(version 依次 +1)即可, 老数据不丢。
bool DBManager::applyMigrations(int fromVersion, QString *err)
{
    auto fail = [&](const QString &msg) {
        if (err) *err = msg;
        return false;
    };

    // 迁移 1: station 表补 code_prefix 字段, 并从已有桩号(如 DR-01)回填前缀
    if (fromVersion < 1) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移1: 开启事务失败"));
        QSqlQuery q(db());
        bool hasPrefix = false;
        if (q.exec(QStringLiteral("PRAGMA table_info(station);"))) {
            while (q.next()) {
                if (q.value(1).toString() == QStringLiteral("code_prefix")) {
                    hasPrefix = true;
                    break;
                }
            }
        }
        if (!hasPrefix) {
            if (!q.exec(QStringLiteral(
                    "ALTER TABLE station ADD COLUMN code_prefix TEXT NOT NULL DEFAULT '';"))) {
                rollbackTransaction();
                return fail(QStringLiteral("迁移1: 添加 code_prefix 失败: %1").arg(q.lastError().text()));
            }
            q.exec(QStringLiteral(
                "UPDATE station SET code_prefix = COALESCE("
                "  (SELECT substr(c.code, 1, instr(c.code, '-') - 1) FROM charger c"
                "    WHERE c.station_id = station.station_id AND instr(c.code, '-') > 0 LIMIT 1), '')"
                " WHERE code_prefix = '';"));
        }
        if (!writeSchemaVersion(1) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移1: 提交失败"));
        }
        qDebug() << "数据库迁移 1 完成(station.code_prefix 补列并回填)。";
        fromVersion = 1;
    }

    // 迁移 2: 电桩状态语义升级 —— 旧值{0空闲,1使用中,2故障} 改为 {0空闲,1已连接,2充电中,3故障}
    if (fromVersion < 2) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移2: 开启事务失败"));
        QSqlQuery q(db());
        // 旧 1(使用中) 对应新 2(充电中); 旧 2(故障) 对应新 3(故障); 旧 0 不变
        if (!q.exec(QStringLiteral(
                "UPDATE charger SET status = CASE status "
                "  WHEN 1 THEN 2 WHEN 2 THEN 3 ELSE status END;"))) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移2: 电桩状态重映射失败: %1").arg(q.lastError().text()));
        }
        if (!writeSchemaVersion(2) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移2: 提交失败"));
        }
        qDebug() << "数据库迁移 2 完成(电桩状态: 0空闲 1已连接 2充电中 3故障)。";
        fromVersion = 2;
    }

    // 迁移 3: 为 BR-02/BR-03 部分唯一索引做数据清理。
    // 旧版没有约束, 可能出现过"同一用户/同一电桩有多条未结算订单", 先取消旧的只留最新,
    // 并释放已经没有任何活动订单的电桩, 否则建唯一索引会失败。
    if (fromVersion < 3) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移3: 开启事务失败"));
        QSqlQuery q(db());
        // 同一用户保留最新一条活动订单, 其余置已取消
        q.exec(QStringLiteral(
            "UPDATE charging_order SET status = 3 WHERE order_id IN ("
            "  SELECT o.order_id FROM charging_order o"
            "   WHERE o.status IN (0,1) AND EXISTS ("
            "     SELECT 1 FROM charging_order o2"
            "      WHERE o2.user_id = o.user_id AND o2.status IN (0,1) AND o2.order_id > o.order_id))"));
        // 同一电桩保留最新一条活动订单, 其余置已取消
        q.exec(QStringLiteral(
            "UPDATE charging_order SET status = 3 WHERE order_id IN ("
            "  SELECT o.order_id FROM charging_order o"
            "   WHERE o.status IN (0,1) AND EXISTS ("
            "     SELECT 1 FROM charging_order o2"
            "      WHERE o2.charger_id = o.charger_id AND o2.status IN (0,1) AND o2.order_id > o.order_id))"));
        // 释放那些已经没有任何活动订单、但仍显示占用中的电桩
        q.exec(QStringLiteral(
            "UPDATE charger SET status = 0 WHERE charger_id NOT IN ("
            "  SELECT charger_id FROM charging_order WHERE status IN (0,1))"
            "  AND status IN (1,2);"));
        if (q.lastError().isValid()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移3: 清理重复订单失败: %1").arg(q.lastError().text()));
        }
        if (!writeSchemaVersion(3) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移3: 提交失败"));
        }
        qDebug() << "数据库迁移 3/3 完成(BR-02/03 唯一索引前清理)。";
        fromVersion = 3;
    }

    // 以后新迁移照此继续加: if (fromVersion < 4) {...}
    return true;
}

// ------------------------- 工具 -------------------------

QString DBManager::nowStr()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

double DBManager::round2(double v)
{
    return std::round(v * 100.0) / 100.0;
}

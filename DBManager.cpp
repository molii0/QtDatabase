#include "DBManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QThreadStorage>

#include <cmath>

// ============================================================================
// DBManager 核心部分: 连接管理 / 建表 / 索引 / 版本迁移 / 初始化
// 建表 SQL 与增量迁移脚本都不再内联在代码里, 而是 db/*.sql 文件,
// 经 Qt 资源(db.qrc)编译进程序, 运行时从 ":/db/..." 读取执行。
// ============================================================================

namespace {

const QString kDefaultDbName = QStringLiteral("charge_platform.db");

// 读取一个 SQL 资源文件并逐条执行
// 规则: 每条语句以 ';' 结尾; 先逐行剥掉 '--' 注释(含行内注释), 再按 ';' 切分,
// 这样注释里即使出现分号也不会干扰切分。
bool execResource(QSqlQuery &q, const QString &resPath, QString *err)
{
    QFile f(resPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("读取 SQL 资源失败: %1").arg(resPath);
        return false;
    }
    const QString text = QString::fromUtf8(f.readAll());

    // 1) 逐行去掉注释
    QStringList lines = text.split(QLatin1Char('\n'));
    QStringList kept;
    for (const QString &line : lines) {
        QString l = line;
        const int c = l.indexOf(QStringLiteral("--"));
        if (c >= 0)
            l = l.left(c);          // 去掉行内 "--" 及其后内容
        if (!l.trimmed().isEmpty())
            kept << l;
    }

    // 2) 按 ';' 切分成一条条语句
    QStringList statements;
    const QString joined = kept.join(QLatin1Char('\n'));
    for (const QString &chunk : joined.split(QLatin1Char(';'))) {
        const QString stmt = chunk.trimmed();
        if (!stmt.isEmpty())
            statements << stmt;
    }

    for (const QString &stmt : statements) {
        if (!q.exec(stmt)) {
            if (err) *err = QStringLiteral("SQL 执行失败(%1): %2")
                                 .arg(resPath, q.lastError().text());
            return false;
        }
    }
    return true;
}

} // namespace

// ------------------------- 单例与生命周期 -------------------------

DBManager &DBManager::instance()
{
    static DBManager mgr;
    return mgr;
}

DBManager::DBManager() = default;

DBManager::~DBManager() = default;

// 默认库路径解析(只算路径, 不建库/不打开) —— 兼容旧用法:
//   ① 当前目录已有 charge_platform.db        -> 用它(旧版在 exe 目录/工作目录跑的习惯)
//   ② exe 目录已有 charge_platform.db        -> 用它
//   ③ 两处都没有                             -> 工程根目录(编译期 DEFAULT_DB_DIR), 
//                                              未定义宏则退回 exe 目录(不存在则建库)
QString DBManager::resolveDefaultDbPath()
{
    const QString cwdDb = QDir(QDir::currentPath()).filePath(kDefaultDbName);
    if (QFile::exists(cwdDb)) {
        qDebug() << "默认库: 当前目录已有库, 直接使用 ->" << cwdDb;
        return cwdDb;
    }
    const QString exeDb = QDir(QCoreApplication::applicationDirPath()).filePath(kDefaultDbName);
    if (QFile::exists(exeDb)) {
        qDebug() << "默认库: 程序目录已有库, 直接使用 ->" << exeDb;
        return exeDb;
    }
#ifdef DEFAULT_DB_DIR
    const QString rootDb = QDir(QString::fromLatin1(DEFAULT_DB_DIR)).filePath(kDefaultDbName);
    qDebug() << "默认库: 以上都没有, 使用工程根目录(不存在会自动建库) ->" << rootDb;
    return rootDb;
#else
    qDebug() << "默认库: 使用程序目录(不存在会自动建库) ->" << exeDb;
    return exeDb;
#endif
}

bool DBManager::init(const QString &dbFilePath)
{
    QString path = dbFilePath;
    if (path.isEmpty())
        path = resolveDefaultDbPath();
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

// 建全部表(内容来自资源 db/schema.sql, 全部 IF NOT EXISTS)
bool DBManager::createTables()
{
    QString err;
    QSqlQuery q(db());
    if (!execResource(q, QStringLiteral(":/db/schema.sql"), &err)) {
        qCritical() << "建表失败:" << err;
        return false;
    }
    return true;
}

// 建索引(内容来自资源 db/indexes.sql, 每次启动执行, 全部 IF NOT EXISTS)
bool DBManager::createIndexes()
{
    QString err;
    QSqlQuery q(db());
    if (!execResource(q, QStringLiteral(":/db/indexes.sql"), &err)) {
        qCritical() << "建索引失败:" << err;
        return false;
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
// 每个迁移的 SQL 在 db/migrations/ 里, 编译进程序资源; 以后结构要变:
// 新建 db/migrations/00N_xxx.sql 并在这里加一步、把 kSchemaVersion +1 即可。
bool DBManager::applyMigrations(int fromVersion, QString *err)
{
    auto fail = [&](const QString &msg) {
        if (err) *err = msg;
        return false;
    };

    // 迁移 1: station 补 code_prefix, 并从桩号回填前缀(仅当列不存在时执行该文件)
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
            QString migErr;
            if (!execResource(q, QStringLiteral(":/db/migrations/001_station_code_prefix.sql"),
                              &migErr)) {
                rollbackTransaction();
                return fail(QStringLiteral("迁移1: %1").arg(migErr));
            }
        }
        if (!writeSchemaVersion(1) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移1: 提交失败"));
        }
        qDebug() << "数据库迁移 1 完成(station.code_prefix)。";
        fromVersion = 1;
    }

    // 迁移 2: 电桩状态语义升级(旧 使用中1→充电中2, 故障2→故障3)
    if (fromVersion < 2) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移2: 开启事务失败"));
        QSqlQuery q(db());
        QString migErr;
        if (!execResource(q, QStringLiteral(":/db/migrations/002_charger_status_remap.sql"),
                          &migErr)) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移2: %1").arg(migErr));
        }
        if (!writeSchemaVersion(2) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移2: 提交失败"));
        }
        qDebug() << "数据库迁移 2 完成(电桩状态: 0空闲 1已连接 2充电中 3故障)。";
        fromVersion = 2;
    }

    // 迁移 3: 加 BR-02/03 唯一索引前, 清理历史重复活动订单
    if (fromVersion < 3) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移3: 开启事务失败"));
        QSqlQuery q(db());
        QString migErr;
        if (!execResource(q, QStringLiteral(":/db/migrations/003_clean_duplicate_orders.sql"),
                          &migErr)) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移3: %1").arg(migErr));
        }
        if (!writeSchemaVersion(3) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移3: 提交失败"));
        }
        qDebug() << "数据库迁移 3 完成(BR-02/03 唯一索引前清理)。";
        fromVersion = 3;
    }

    // 迁移 4: user 表补 avatar 字段(C端用户资料)
    if (fromVersion < 4) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移4: 开启事务失败"));
        QSqlQuery q(db());
        QString migErr;
        if (!execResource(q, QStringLiteral(":/db/migrations/004_add_user_avatar.sql"),
                          &migErr)) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移4: %1").arg(migErr));
        }
        if (!writeSchemaVersion(4) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移4: 提交失败"));
        }
        qDebug() << "数据库迁移 4 完成(user.avatar)。";
        fromVersion = 4;
    }

    // 迁移 5: charging_order 补 paid/debt(BR-06 余额不足允许结算并记欠费)
    if (fromVersion < 5) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移5: 开启事务失败"));
        QSqlQuery q(db());
        QString migErr;
        if (!execResource(q, QStringLiteral(":/db/migrations/005_add_order_paid_debt.sql"),
                          &migErr)) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移5: %1").arg(migErr));
        }
        if (!writeSchemaVersion(5) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移5: 提交失败"));
        }
        qDebug() << "数据库迁移 5 完成(订单 paid/debt)。";
        fromVersion = 5;
    }

    // 迁移 6: user 补 debt(未结清欠费), 并把旧订单欠费并入用户欠费
    if (fromVersion < 6) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移6: 开启事务失败"));
        QSqlQuery q(db());
        QString migErr;
        if (!execResource(q, QStringLiteral(":/db/migrations/006_add_user_debt.sql"),
                          &migErr)) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移6: %1").arg(migErr));
        }
        if (!writeSchemaVersion(6) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移6: 提交失败"));
        }
        qDebug() << "数据库迁移 6 完成(user.debt 未结清欠费)。";
        fromVersion = 6;
    }

    // 迁移 7: 设备接入表(遥测/心跳/命令通道, Charger Simulator 对接)
    if (fromVersion < 7) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移7: 开启事务失败"));
        QSqlQuery q(db());
        QString migErr;
        if (!execResource(q, QStringLiteral(":/db/migrations/007_device_access.sql"),
                          &migErr)) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移7: %1").arg(migErr));
        }
        if (!writeSchemaVersion(7) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移7: 提交失败"));
        }
        qDebug() << "数据库迁移 7 完成(设备接入表)。";
        fromVersion = 7;
    }

    // 迁移 8: 分时电价 station.price_peak/price_valley(峰谷平, 原 price 视为平段价)
    if (fromVersion < 8) {
        if (!beginTransaction())
            return fail(QStringLiteral("迁移8: 开启事务失败"));
        QSqlQuery q(db());
        QString migErr;
        if (!execResource(q, QStringLiteral(":/db/migrations/008_time_of_use_price.sql"),
                          &migErr)) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移8: %1").arg(migErr));
        }
        if (!writeSchemaVersion(8) || !commitTransaction()) {
            rollbackTransaction();
            return fail(QStringLiteral("迁移8: 提交失败"));
        }
        qDebug() << "数据库迁移 8 完成(分时电价: 峰/平/谷)。";
        fromVersion = 8;
    }

    // 以后新迁移照此继续加: if (fromVersion < 9) {... 执行 /db/migrations/009_*.sql ...}
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

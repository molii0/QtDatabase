#include "DBManager.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>

// ============================================================================
// 用户(user) 与 管理员(admin) 的数据操作
// ============================================================================

bool DBManager::insertUser(const QString &phone, const QString &nickname)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("INSERT INTO user (phone, nickname) VALUES (:p, :n);"));
    q.bindValue(QStringLiteral(":p"), phone);
    q.bindValue(QStringLiteral(":n"), nickname);
    if (!q.exec()) {
        // 手机号重复时数据库会报 UNIQUE 错误
        qDebug() << "注册用户失败(手机号可能已存在):" << q.lastError().text();
        return false;
    }
    return true;
}

bool DBManager::getUserByPhone(const QString &phone, User *out)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT user_id, phone, nickname, balance, status, register_time "
                             "FROM user WHERE phone = :p;"));
    q.bindValue(QStringLiteral(":p"), phone);
    if (!q.exec() || !q.next())
        return false;   // 查不到该手机号
    if (out) {
        out->userId = q.value(0).toLongLong();
        out->phone = q.value(1).toString();
        out->nickname = q.value(2).toString();
        out->balance = q.value(3).toDouble();
        out->status = q.value(4).toInt();
        out->registerTime = q.value(5).toString();
    }
    return true;
}

bool DBManager::updateBalance(qint64 userId, double delta)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE user SET balance = balance + :d WHERE user_id = :id;"));
    q.bindValue(QStringLiteral(":d"), delta);
    q.bindValue(QStringLiteral(":id"), userId);
    return q.exec() && q.numRowsAffected() > 0;
}

// 管理端用户列表; phoneKeyword 为空返回全部, 否则按手机号模糊搜索
bool DBManager::getUserById(qint64 userId, User *out) const
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT user_id, phone, nickname, balance, status, register_time "
                             "FROM user WHERE user_id = :id;"));
    q.bindValue(QStringLiteral(":id"), userId);
    if (!q.exec() || !q.next())
        return false;
    if (out) {
        out->userId = q.value(0).toLongLong();
        out->phone = q.value(1).toString();
        out->nickname = q.value(2).toString();
        out->balance = q.value(3).toDouble();
        out->status = q.value(4).toInt();
        out->registerTime = q.value(5).toString();
    }
    return true;
}

bool DBManager::listUsers(const QString &phoneKeyword, QVector<User> *out) const
{
    out->clear();
    QSqlQuery q(db());
    // 注意: 空字符串参数在 SQLite 驱动里会被绑成 NULL, 所以"查全部"用无参 SQL 分支
    if (phoneKeyword.trimmed().isEmpty()) {
        q.exec(QStringLiteral("SELECT user_id, phone, nickname, balance, status, register_time "
                              "FROM user ORDER BY user_id;"));
    } else {
        q.prepare(QStringLiteral(
            "SELECT user_id, phone, nickname, balance, status, register_time FROM user"
            " WHERE phone LIKE :pat ORDER BY user_id;"));
        q.bindValue(QStringLiteral(":pat"),
                    QStringLiteral("%%1%").arg(phoneKeyword.trimmed()));
        q.exec();
    }
    if (q.lastError().isValid())
        return false;
    while (q.next()) {
        User u;
        u.userId = q.value(0).toLongLong();
        u.phone = q.value(1).toString();
        u.nickname = q.value(2).toString();
        u.balance = q.value(3).toDouble();
        u.status = q.value(4).toInt();
        u.registerTime = q.value(5).toString();
        out->append(u);
    }
    return true;
}

// frozen = true 冻结 / false 解冻(冻结用户不能登录、不能开始新充电)
bool DBManager::setUserStatus(qint64 userId, bool frozen, QString *err)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE user SET status = :st WHERE user_id = :id;"));
    q.bindValue(QStringLiteral(":st"), frozen ? 0 : 1);
    q.bindValue(QStringLiteral(":id"), userId);
    if (!q.exec() || q.numRowsAffected() != 1) {
        if (err) *err = QStringLiteral("用户不存在 (userId=%1)").arg(userId);
        return false;
    }
    return true;
}

bool DBManager::checkAdminLogin(const QString &account, const QString &password)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT password FROM admin WHERE account = :a;"));
    q.bindValue(QStringLiteral(":a"), account);
    if (!q.exec() || !q.next())
        return false;
    return q.value(0).toString() == password;
}

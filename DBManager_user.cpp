#include "DBManager.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>

// ============================================================================
// 用户(user) 与 管理员(admin) 的数据操作
// user 表列顺序: user_id, phone, nickname, avatar, balance, status, register_time, debt
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

// 用户行公共列(含未结清欠费 debt)
static const char kUserCols[] =
    "SELECT user_id, phone, nickname, avatar, balance, status, register_time, debt FROM user ";

static void fillUser(QSqlQuery &q, DBManager::User &u)
{
    u.userId = q.value(0).toLongLong();
    u.phone = q.value(1).toString();
    u.nickname = q.value(2).toString();
    u.avatar = q.value(3).toString();
    u.balance = q.value(4).toDouble();
    u.status = q.value(5).toInt();
    u.registerTime = q.value(6).toString();
    u.debt = q.value(7).toDouble();
}

bool DBManager::getUserByPhone(const QString &phone, User *out)
{
    QSqlQuery q(db());
    q.prepare(QString::fromLatin1(kUserCols) + QStringLiteral("WHERE phone = :p;"));
    q.bindValue(QStringLiteral(":p"), phone);
    if (!q.exec() || !q.next())
        return false;   // 查不到该手机号
    if (out)
        fillUser(q, *out);
    return true;
}

bool DBManager::getUserById(qint64 userId, User *out) const
{
    QSqlQuery q(db());
    q.prepare(QString::fromLatin1(kUserCols) + QStringLiteral("WHERE user_id = :id;"));
    q.bindValue(QStringLiteral(":id"), userId);
    if (!q.exec() || !q.next())
        return false;
    if (out)
        fillUser(q, *out);
    return true;
}

bool DBManager::listUsers(const QString &phoneKeyword, QVector<User> *out) const
{
    out->clear();
    QSqlQuery q(db());
    // 注意: 空字符串参数在 SQLite 驱动里会被绑成 NULL, 所以"查全部"用无参 SQL 分支
    if (phoneKeyword.trimmed().isEmpty()) {
        q.exec(QString::fromLatin1(kUserCols) + QStringLiteral("ORDER BY user_id;"));
    } else {
        q.prepare(QString::fromLatin1(kUserCols)
                  + QStringLiteral("WHERE phone LIKE :pat ORDER BY user_id;"));
        q.bindValue(QStringLiteral(":pat"),
                    QStringLiteral("%%1%").arg(phoneKeyword.trimmed()));
        q.exec();
    }
    if (q.lastError().isValid())
        return false;
    while (q.next()) {
        User u;
        fillUser(q, u);
        out->append(u);
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

// 充值(推荐接口): 事务内"先还欠费, 剩余进余额"
//   repayOut       本次充值中用于还欠费的钱
//   remainDebtOut  还完后仍未结清的欠费
bool DBManager::recharge(qint64 userId, double amount,
                         double *repayOut, double *remainDebtOut, QString *err)
{
    if (amount <= 0) {
        if (err) *err = QStringLiteral("充值金额必须大于 0");
        return false;
    }
    if (!beginTransaction()) {
        if (err) *err = QStringLiteral("开启事务失败");
        return false;
    }
    const auto rollbackAndFail = [&](const QString &msg) {
        if (err) *err = msg;
        rollbackTransaction();
        return false;
    };
    const QSqlDatabase dbc = db();

    QSqlQuery q(dbc);
    q.prepare(QStringLiteral("SELECT balance, debt FROM user WHERE user_id = :id;"));
    q.bindValue(QStringLiteral(":id"), userId);
    if (!q.exec() || !q.next())
        return rollbackAndFail(QStringLiteral("用户不存在 (userId=%1)").arg(userId));

    const double oldBalance = q.value(0).toDouble();
    const double oldDebt = q.value(1).toDouble();
    const double repay = qMin(amount, oldDebt);          // 先还欠费
    const double toBalance = round2(amount - repay);     // 多余部分进余额
    const double newDebt = round2(oldDebt - repay);
    const double newBalance = round2(oldBalance + toBalance);

    q.prepare(QStringLiteral(
        "UPDATE user SET balance = :b, debt = :d WHERE user_id = :id;"));
    q.bindValue(QStringLiteral(":b"), newBalance);
    q.bindValue(QStringLiteral(":d"), newDebt);
    q.bindValue(QStringLiteral(":id"), userId);
    if (!q.exec() || q.numRowsAffected() != 1) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("充值失败"));
    }
    if (!commitTransaction()) {
        rollbackTransaction();
        return rollbackAndFail(QStringLiteral("提交事务失败"));
    }
    if (repayOut) *repayOut = repay;
    if (remainDebtOut) *remainDebtOut = newDebt;
    qDebug() << "充值: 用户" << userId << "还欠费" << repay
             << "元, 到账" << toBalance << "元, 余额" << newBalance
             << ", 剩余欠费" << newDebt;
    return true;
}

bool DBManager::updateNickname(qint64 userId, const QString &nickname, QString *err)
{
    if (nickname.trimmed().isEmpty()) {
        if (err) *err = QStringLiteral("昵称不能为空");
        return false;
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE user SET nickname = :n WHERE user_id = :id;"));
    q.bindValue(QStringLiteral(":n"), nickname.trimmed());
    q.bindValue(QStringLiteral(":id"), userId);
    if (!q.exec() || q.numRowsAffected() != 1) {
        if (err) *err = QStringLiteral("用户不存在 (userId=%1)").arg(userId);
        return false;
    }
    return true;
}

bool DBManager::updateAvatar(qint64 userId, const QString &avatar, QString *err)
{
    // avatar 存"相对路径/文件名", 空串表示清除头像(用默认灰头像)
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE user SET avatar = :a WHERE user_id = :id;"));
    q.bindValue(QStringLiteral(":a"), avatar.isNull() ? QStringLiteral("") : avatar);
    q.bindValue(QStringLiteral(":id"), userId);
    if (!q.exec() || q.numRowsAffected() != 1) {
        if (err) *err = QStringLiteral("用户不存在 (userId=%1)").arg(userId);
        return false;
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

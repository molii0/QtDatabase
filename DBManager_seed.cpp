#include "DBManager.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QTime>
#include <QVector>

// ============================================================================
// 演示数据(种子数据)—— 只在数据库是空库时执行一次
//   1 个管理员 / 5 个用户(含 1 个冻结) / 5 座充电站 / 每站 6 台桩 /
//   近 3 天约 30 条已完成订单 / 1 条正在充电的订单
// ============================================================================

namespace {

// 电站信息
struct StationSeed {
    const char *name;
    const char *abbr;       // 站点缩写, 用于电桩编号如 DR-01
    const char *address;
    double lng;
    double lat;
    double price;           // 元/度
};

const StationSeed kStations[] = {
    {"东软软件园充电站(浑南)", "DR", "沈阳市浑南区新秀街2号东软软件园", 123.4512, 41.7098, 1.20},
    {"奥体中心充电站(浑南)",   "AT", "沈阳市浑南区浑南中路与营盘街交汇处", 123.4698, 41.7430, 1.30},
    {"沈阳站前充电站(和平)",   "SZ", "沈阳市和平区胜利南街55号",           123.3969, 41.8010, 1.35},
    {"桃仙机场充电站(浑南)",   "TX", "沈阳市浑南区桃仙国际机场T3停车楼",   123.4779, 41.6410, 1.50},
    {"星海广场充电站(大连)",   "XH", "大连市沙河口区星海广场地下停车场",   121.5953, 38.8800, 1.40},
};
constexpr int kStationCount = 5;

// 用户信息
struct UserSeed {
    const char *phone;
    const char *nickname;
    double balance;
    bool frozen;        // 是否冻结用户(演示管理端冻结功能)
};

const UserSeed kUsers[] = {
    {"12345678910", "测试用户", 88.50, false},
    {"13900000001", "用户0001", 120.00, false},
    {"13900000002", "用户0002", 6.00,  false},
    {"13900000003", "用户0003", 45.80, false},
    {"13900000004", "用户0004", 200.00, true},
};
constexpr int kUserCount = 5;

// 一台桩的插入结果(后面生成订单要用到桩的功率)
struct Pile {
    qint64 chargerId = 0;
    double power = 0.0;
};

QString ts(int year, int month, int day, int hour, int minute)
{
    return QDateTime(QDate(year, month, day), QTime(hour, minute))
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

} // namespace

bool DBManager::seedData()
{
    QSqlDatabase dbc = db();

    // 整体用一个事务包住: 任何一步失败就回滚, 不留半截数据
    if (!dbc.transaction()) {
        qDebug() << "种子数据: 开启事务失败";
        return false;
    }
    QSqlQuery q(dbc);

    // ---------- 1. 管理员: admin / 123456 ----------
    q.prepare(QStringLiteral("INSERT INTO admin (account, password) VALUES (:a, :p);"));
    q.bindValue(QStringLiteral(":a"), QStringLiteral("admin"));
    q.bindValue(QStringLiteral(":p"), QStringLiteral("123456"));
    if (!q.exec()) { qDebug() << "种子数据: 插入管理员失败" << q.lastError().text(); dbc.rollback(); return false; }

    // ---------- 2. 用户(注册时间用当前时间往前推 20+i 天) ----------
    QVector<qint64> userIds;
    for (int i = 0; i < kUserCount; ++i) {
        const UserSeed &u = kUsers[i];
        const QString regTime = QDateTime::currentDateTime()
                                    .addDays(-(20 + i))
                                    .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        q.prepare(QStringLiteral("INSERT INTO user (phone, nickname, balance, status, register_time) "
                                 "VALUES (:p, :n, :b, :s, :r);"));
        q.bindValue(QStringLiteral(":p"), QString::fromUtf8(u.phone));
        q.bindValue(QStringLiteral(":n"), QString::fromUtf8(u.nickname));
        q.bindValue(QStringLiteral(":b"), u.balance);
        q.bindValue(QStringLiteral(":s"), u.frozen ? 0 : 1);
        q.bindValue(QStringLiteral(":r"), regTime);
        if (!q.exec()) { qDebug() << "种子数据: 插入用户失败" << q.lastError().text(); dbc.rollback(); return false; }
        userIds.append(q.lastInsertId().toLongLong());
    }

    // ---------- 3. 充电站(带缩写, 供电桩编号) ----------
    QVector<qint64> stationIds;
    for (const StationSeed &s : kStations) {
        q.prepare(QStringLiteral("INSERT INTO station (name, code_prefix, address, longitude, latitude, price) "
                                 "VALUES (:n, :cp, :a, :lng, :lat, :p);"));
        q.bindValue(QStringLiteral(":n"), QString::fromUtf8(s.name));
        q.bindValue(QStringLiteral(":cp"), QString::fromUtf8(s.abbr));
        q.bindValue(QStringLiteral(":a"), QString::fromUtf8(s.address));
        q.bindValue(QStringLiteral(":lng"), s.lng);
        q.bindValue(QStringLiteral(":lat"), s.lat);
        q.bindValue(QStringLiteral(":p"), s.price);
        if (!q.exec()) { qDebug() << "种子数据: 插入充电站失败" << q.lastError().text(); dbc.rollback(); return false; }
        stationIds.append(q.lastInsertId().toLongLong());
    }

    // ---------- 4. 充电桩: 每站 6 台(编号 缩写-01~06) ----------
    //    编号奇数位是快充(120/60 kW), 偶数位是慢充 7kW; 每站第 6 台是故障桩
    QVector<QVector<Pile>> allPiles;   // allPiles[电站下标] = 该站的 6 台桩
    for (int si = 0; si < kStationCount; ++si) {
        QVector<Pile> piles;
        for (int j = 1; j <= 6; ++j) {
            const bool fast = (j % 2 == 1);
            const double power = fast ? (j == 1 ? 120.0 : 60.0) : 7.0;
            const int status = (j == 6) ? ChargerFault : ChargerIdle;   // 第 6 台故障
            const QString code = QString::fromUtf8(kStations[si].abbr) + QStringLiteral("-")
                                 + QString::number(j).rightJustified(2, QLatin1Char('0'));

            q.prepare(QStringLiteral("INSERT INTO charger (station_id, code, type, power, status) "
                                     "VALUES (:sid, :code, :type, :pw, :st);"));
            q.bindValue(QStringLiteral(":sid"), stationIds.at(si));
            q.bindValue(QStringLiteral(":code"), code);
            q.bindValue(QStringLiteral(":type"), fast ? 1 : 0);
            q.bindValue(QStringLiteral(":pw"), power);
            q.bindValue(QStringLiteral(":st"), status);
            if (!q.exec()) { qDebug() << "种子数据: 插入充电桩失败" << q.lastError().text(); dbc.rollback(); return false; }
            piles.append(Pile{q.lastInsertId().toLongLong(), power});
        }
        allPiles.append(piles);
    }

    // ---------- 5. 近 3 天的已完成订单(给管理端营收页面看数据) ----------
    //    时间/电桩/用户都按简单公式错开, 保证能对上外键
    qint64 orderSeq = 0;
    QDate today = QDate::currentDate();
    for (int si = 0; si < kStationCount; ++si) {
        const double price = kStations[si].price;
        for (int day = 3; day >= 1; --day) {          // 昨天、前天、大前天
            for (int k = 0; k < 2; ++k) {             // 每天 2 单
                const int pileIdx = (si + day + k) % 5;                 // 0~4, 避开故障的第 6 台
                const int userIdx = (si * 2 + day + k) % 4;             // 前 4 个用户轮流
                const double power = allPiles.at(si).at(pileIdx).power;
                const int hour = 8 + (si * 3 + day + k * 5) % 12;       // 8~19 点
                const int minute = (si * 11 + day * 7 + k * 13) % 60;
                // 一次充电时长按桩的功率区分, 更贴近现实:
                //   120kW 快充 10~40 分钟, 60kW 快充 20~80 分钟, 7kW 慢充 30~180 分钟
                const int f = si * 17 + day * 13 + k * 29;
                const int durMin = (power >= 100.0) ? 10 + f % 31
                                 : (power >= 50.0)  ? 20 + f % 61
                                                    : 30 + f % 151;

                const QDate d = today.addDays(-day);
                const QString startTs = ts(d.year(), d.month(), d.day(), hour, minute);
                const QString endTs = QDateTime::fromString(startTs, QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                          .addSecs(durMin * 60)
                                          .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

                const double energy = round2(power * durMin / 60.0);   // 电量 = 功率×时长
                const double amount = round2(energy * price);          // 费用 = 电量×单价

                q.prepare(QStringLiteral(
                    "INSERT INTO charging_order (order_no, user_id, station_id, charger_id, status,"
                    "                            energy, amount, start_time, end_time)"
                    " VALUES (:no, :uid, :sid, :cid, 2, :en, :am, :st, :et);"));
                q.bindValue(QStringLiteral(":no"), QStringLiteral("T%1").arg(++orderSeq, 5, 10, QLatin1Char('0')));
                q.bindValue(QStringLiteral(":uid"), userIds.at(userIdx));
                q.bindValue(QStringLiteral(":sid"), stationIds.at(si));
                q.bindValue(QStringLiteral(":cid"), allPiles.at(si).at(pileIdx).chargerId);
                q.bindValue(QStringLiteral(":en"), energy);
                q.bindValue(QStringLiteral(":am"), amount);
                q.bindValue(QStringLiteral(":st"), startTs);
                q.bindValue(QStringLiteral(":et"), endTs);
                if (!q.exec()) { qDebug() << "种子数据: 插入历史订单失败" << q.lastError().text(); dbc.rollback(); return false; }
            }
        }
    }

    // ---------- 6. 一条"正在充电"的订单(演示结算流程) ----------
    //    测试用户 在 东软软件园 DR-01(快充 120kW)上充了 15 分钟
    const Pile activePile = allPiles.first().first();   // DR-01
    const QString startTs = QDateTime::currentDateTime()
                                .addSecs(-15 * 60)
                                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    q.prepare(QStringLiteral(
        "INSERT INTO charging_order (order_no, user_id, station_id, charger_id, status,"
        "                            energy, amount, start_time, end_time)"
        " VALUES (:no, :uid, :sid, :cid, 1, 0, 0, :st, NULL);"));
    q.bindValue(QStringLiteral(":no"), QStringLiteral("T%1").arg(++orderSeq, 5, 10, QLatin1Char('0')));
    q.bindValue(QStringLiteral(":uid"), userIds.at(0));          // 测试用户
    q.bindValue(QStringLiteral(":sid"), stationIds.first());
    q.bindValue(QStringLiteral(":cid"), activePile.chargerId);
    q.bindValue(QStringLiteral(":st"), startTs);
    if (!q.exec()) { qDebug() << "种子数据: 插入充电中订单失败" << q.lastError().text(); dbc.rollback(); return false; }

    // 对应电桩置为"充电中"(新状态: 2=充电中)
    q.prepare(QStringLiteral("UPDATE charger SET status = :st WHERE charger_id = :id;"));
    q.bindValue(QStringLiteral(":st"), ChargerCharging);
    q.bindValue(QStringLiteral(":id"), activePile.chargerId);
    if (!q.exec()) { qDebug() << "种子数据: 更新电桩状态失败" << q.lastError().text(); dbc.rollback(); return false; }

    if (!dbc.commit()) {
        qDebug() << "种子数据: 提交事务失败";
        dbc.rollback();
        return false;
    }
    return true;
}

#include "DBManager.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QTime>
#include <QVariant>
#include <QVector>

// ============================================================================
// 演示数据(种子数据)—— 只在数据库是空库时执行一次
//   1 个管理员 / 50 个用户(含 2 个冻结) / 50 座充电站 / 每站 6 台桩(共 300) /
//   近 30 天按"真实分布"生成的历史订单(约 7000+, 周末单量高、早晚高峰集中) /
//   1 条"正在充电"的订单(演示结算)
// 规模说明: 约等于原种子(5 用户/5 站/30 桩/约 750 单)的 10 倍,
//   电站分布覆盖沈阳各区与大连, 便于地图/列表/排行图表都有足够数据可看。
// 随机数使用固定种子: 同样的代码重建库, 生成的数据一模一样, 方便复现。
// ============================================================================

namespace {

// 电站信息
struct StationSeed {
    QString name;
    QString abbr;       // 站点缩写, 用于电桩编号如 DR-01
    QString address;
    double lng;
    double lat;
    double price;       // 元/度
};

// 用户信息
struct UserSeed {
    QString phone;
    QString nickname;
    double balance;
    bool frozen;        // 是否冻结用户(演示管理端冻结功能)
};

constexpr int kStationCount = 50;   // 原 5 座真站 + 生成的 45 座
constexpr int kUserCount    = 50;   // 原 5 个 + 生成的 45 个
constexpr int kHistoryDays  = 30;   // 近 30 天历史订单(UC-D-02)

// 前 5 个固定用户(手机号与角色稳定, 自检/演示依赖):
//   12345678910 测试用户 / 13900000001..04(其中 0004 冻结)
const UserSeed kBaseUsers[] = {
    {QStringLiteral("12345678910"), QStringLiteral("测试用户"),  88.50, false},
    {QStringLiteral("13900000001"), QStringLiteral("用户0001"), 120.00, false},
    {QStringLiteral("13900000002"), QStringLiteral("用户0002"),   6.00, false},
    {QStringLiteral("13900000003"), QStringLiteral("用户0003"),  45.80, false},
    {QStringLiteral("13900000004"), QStringLiteral("用户0004"), 200.00, true},
};

// 生成的 45 个用户: 手机号 13700000005..13700000049(不占用测试用的 139/138 号段)
QVector<UserSeed> buildExtraUsers()
{
    QVector<UserSeed> v;
    v.reserve(kUserCount - 5);
    for (int i = 5; i < kUserCount; ++i) {
        UserSeed u;
        u.phone    = QStringLiteral("137%1").arg(i, 8, 10, QChar('0')); // 11 位
        u.nickname = QStringLiteral("用户%1").arg(i, 4, 10, QChar('0'));
        u.balance  = 6.0 + (i * 61) % 394;   // 6 ~ 399 元(整数便于阅读)
        u.frozen   = (i == 29);              // 多演示一个冻结用户(13700000029)
        v.append(u);
    }
    return v;
}

// 50 座电站 = 原来的 5 座真站 + 按"沈阳各区/大连 × 常见场所"生成的 45 座。
// 生成规则固定、无随机数, 重建库结果一致; 代码前缀两字母唯一。
QVector<StationSeed> buildStations()
{
    QVector<StationSeed> v;
    v.reserve(kStationCount);

    // 1) 原来的 5 座真站(保留原名与坐标, 用于"附近电站/地图"演示)
    struct Base { const char *name; const char *abbr; const char *address;
                  double lng, lat, price; };
    const Base base[] = {
        {"东软软件园充电站(浑南)", "DR", "沈阳市浑南区新秀街2号东软软件园", 123.4512, 41.7098, 1.20},
        {"奥体中心充电站(浑南)",   "AT", "沈阳市浑南区浑南中路与营盘街交汇处", 123.4698, 41.7430, 1.30},
        {"沈阳站前充电站(和平)",   "SZ", "沈阳市和平区胜利南街55号",           123.3969, 41.8010, 1.35},
        {"桃仙机场充电站(浑南)",   "TX", "沈阳市浑南区桃仙国际机场T3停车楼",   123.4779, 41.6410, 1.50},
        {"星海广场充电站(大连)",   "XH", "大连市沙河口区星海广场地下停车场",   121.5953, 38.8800, 1.40},
    };
    for (const Base &b : base) {
        StationSeed s;
        s.name    = QString::fromUtf8(b.name);
        s.abbr    = QString::fromUtf8(b.abbr);
        s.address = QString::fromUtf8(b.address);
        s.lng = b.lng; s.lat = b.lat; s.price = b.price;
        v.append(s);
    }

    // 2) 生成的 45 座: 9 个城区锚点 × 5 类场所
    struct Anchor { const char *shortName; const char *cityPrefix; double lng, lat; };
    const Anchor anchors[] = {
        {"浑南",   "沈阳市浑南区",   123.45, 41.70},
        {"和平",   "沈阳市和平区",   123.40, 41.79},
        {"沈河",   "沈阳市沈河区",   123.46, 41.80},
        {"皇姑",   "沈阳市皇姑区",   123.42, 41.83},
        {"铁西",   "沈阳市铁西区",   123.35, 41.80},
        {"大东",   "沈阳市大东区",   123.47, 41.81},
        {"沈北",   "沈阳市沈北新区", 123.42, 41.93},
        {"高新",   "大连市高新园区", 121.53, 38.86},
        {"甘井子", "大连市甘井子区", 121.59, 38.95},
    };
    const char *venues[] = {"万达广场", "万象汇", "大悦城", "吾悦广场", "山姆会员店"};

    int seq = 0;
    for (int a = 0; a < 9; ++a) {
        for (int ven = 0; ven < 5; ++ven) {
            StationSeed s;
            s.name    = QStringLiteral("%1%2充电站")
                            .arg(QString::fromUtf8(anchors[a].shortName),
                                 QString::fromUtf8(venues[ven]));
            // 两字母缩写 AA..IE(9 城区 × 5 场所两两不同), 用于桩编号如 AA-01
            s.abbr    = QString(QChar::fromLatin1(static_cast<char>('A' + a)))
                        + QChar::fromLatin1(static_cast<char>('A' + ven));
            s.address = QStringLiteral("%1%2地下停车场")
                            .arg(QString::fromUtf8(anchors[a].cityPrefix),
                                 QString::fromUtf8(venues[ven]));
            // 坐标在锚点附近做固定小幅偏移, 让地图上的点散开
            s.lng = anchors[a].lng + (ven - 2) * 0.010;
            s.lat = anchors[a].lat + ((a + ven) % 3 - 1) * 0.008;
            s.price = 0.95 + (seq++ % 6) * 0.10;                     // 0.95~1.45
            v.append(s);
        }
    }
    return v;
}

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

// 一台桩的功率: 编号奇数为快充(第1台 120kW, 其余 60kW), 偶数为慢充 7kW
double pilePower(int pileNumber)
{
    const bool fast = (pileNumber % 2 == 1);
    return fast ? (pileNumber == 1 ? 120.0 : 60.0) : 7.0;
}

// 在"当天能充完"的前提下, 按充电高峰权重随机取一个开始分钟(0..1439)
// 晚高峰(17-21点)最忙, 早高峰(6-9点)次之, 深更半夜最少
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

} // namespace

bool DBManager::seedData()
{
    QSqlDatabase dbc = db();
    QRandomGenerator rng(Q_UINT64_C(20260903));   // 固定种子, 数据可复现

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
    const QVector<UserSeed> users = buildExtraUsers();
    QVector<qint64> userIds;          // 全部用户
    QVector<qint64> activeUserIds;    // 未冻结用户(历史订单从中选)
    userIds.reserve(kUserCount);
    activeUserIds.reserve(kUserCount);

    for (int i = 0; i < 5; ++i) {                    // 前 5 个固定用户
        const UserSeed &u = kBaseUsers[i];
        const QString regTime = QDateTime::currentDateTime()
                                    .addDays(-(20 + i))
                                    .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        q.prepare(QStringLiteral("INSERT INTO user (phone, nickname, balance, status, register_time) "
                                 "VALUES (:p, :n, :b, :s, :r);"));
        q.bindValue(QStringLiteral(":p"), u.phone);
        q.bindValue(QStringLiteral(":n"), u.nickname);
        q.bindValue(QStringLiteral(":b"), u.balance);
        q.bindValue(QStringLiteral(":s"), u.frozen ? 0 : 1);
        q.bindValue(QStringLiteral(":r"), regTime);
        if (!q.exec()) { qDebug() << "种子数据: 插入用户失败" << q.lastError().text(); dbc.rollback(); return false; }
        const qint64 id = q.lastInsertId().toLongLong();
        userIds.append(id);
        if (!u.frozen) activeUserIds.append(id);
    }
    for (int i = 0; i < users.size(); ++i) {         // 生成的 45 个用户
        const UserSeed &u = users.at(i);
        const QString regTime = QDateTime::currentDateTime()
                                    .addDays(-(25 + i))
                                    .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        q.prepare(QStringLiteral("INSERT INTO user (phone, nickname, balance, status, register_time) "
                                 "VALUES (:p, :n, :b, :s, :r);"));
        q.bindValue(QStringLiteral(":p"), u.phone);
        q.bindValue(QStringLiteral(":n"), u.nickname);
        q.bindValue(QStringLiteral(":b"), u.balance);
        q.bindValue(QStringLiteral(":s"), u.frozen ? 0 : 1);
        q.bindValue(QStringLiteral(":r"), regTime);
        if (!q.exec()) { qDebug() << "种子数据: 插入用户失败" << q.lastError().text(); dbc.rollback(); return false; }
        const qint64 id = q.lastInsertId().toLongLong();
        userIds.append(id);
        if (!u.frozen) activeUserIds.append(id);
    }

    // ---------- 3. 充电站(带缩写, 供电桩编号) ----------
    const QVector<StationSeed> kStations = buildStations();
    QVector<qint64> stationIds;
    stationIds.reserve(kStationCount);
    for (const StationSeed &s : kStations) {
        q.prepare(QStringLiteral("INSERT INTO station (name, code_prefix, address, longitude, latitude, price) "
                                 "VALUES (:n, :cp, :a, :lng, :lat, :p);"));
        q.bindValue(QStringLiteral(":n"), s.name);
        q.bindValue(QStringLiteral(":cp"), s.abbr);
        q.bindValue(QStringLiteral(":a"), s.address);
        q.bindValue(QStringLiteral(":lng"), s.lng);
        q.bindValue(QStringLiteral(":lat"), s.lat);
        q.bindValue(QStringLiteral(":p"), s.price);
        if (!q.exec()) { qDebug() << "种子数据: 插入充电站失败" << q.lastError().text(); dbc.rollback(); return false; }
        stationIds.append(q.lastInsertId().toLongLong());
    }

    // ---------- 4. 充电桩: 每站 6 台(编号 缩写-01~06), 共 300 台 ----------
    //    第 6 台是故障桩, 其余空闲(第 1 台 120kW 快充演示用)
    QVector<QVector<Pile>> allPiles;   // allPiles[电站下标] = 该站的 6 台桩
    allPiles.reserve(kStationCount);
    for (int si = 0; si < kStationCount; ++si) {
        QVector<Pile> piles;
        for (int j = 1; j <= 6; ++j) {
            const double power = pilePower(j);
            const int status = (j == 6) ? ChargerFault : ChargerIdle;
            const QString code = kStations.at(si).abbr + QStringLiteral("-")
                                 + QString::number(j).rightJustified(2, QLatin1Char('0'));

            q.prepare(QStringLiteral("INSERT INTO charger (station_id, code, type, power, status) "
                                     "VALUES (:sid, :code, :type, :pw, :st);"));
            q.bindValue(QStringLiteral(":sid"), stationIds.at(si));
            q.bindValue(QStringLiteral(":code"), code);
            q.bindValue(QStringLiteral(":type"), (j % 2 == 1) ? 1 : 0);
            q.bindValue(QStringLiteral(":pw"), power);
            q.bindValue(QStringLiteral(":st"), status);
            if (!q.exec()) { qDebug() << "种子数据: 插入充电桩失败" << q.lastError().text(); dbc.rollback(); return false; }
            piles.append(Pile{q.lastInsertId().toLongLong(), power});
        }
        allPiles.append(piles);
    }

    // ---------- 5. 近 30 天历史订单(给营收图/ML 训练有数据可看) ----------
    //    单量密度不变(每站每天约 3~7 单), 站数 ×10, 总量约 7000+ 单
    qint64 orderSeq = 0;
    QDate today = QDate::currentDate();
    q.prepare(QStringLiteral(
        "INSERT INTO charging_order (order_no, user_id, station_id, charger_id, status,"
        "                            energy, amount, start_time, end_time)"
        " VALUES (:no, :uid, :sid, :cid, :st, :en, :am, :sdt, :edt);"));

    for (int dayAgo = kHistoryDays; dayAgo >= 1; --dayAgo) {
        const QDate day = today.addDays(-dayAgo);
        const int dow = day.dayOfWeek();                 // 周一=1 ... 周日=7
        const bool weekend = (dow >= 6);                 // 周六周日充电需求更高

        for (int si = 0; si < kStationCount; ++si) {
            const double price = kStations.at(si).price;
            // 每天的订单量: 平时 3~6 单, 周末 3~7 单(随机波动, 周末略高)
            // (50 站 × 30 天 × 约 4.7 单 ≈ 7000+, 约为原种子的 10 倍)
            const int count = 3 + rng.bounded(weekend ? 5 : 4);

            for (int k = 0; k < count; ++k) {
                const int userIdx = rng.bounded(activeUserIds.size());   // 未冻结用户轮流
                const int pileIdx = 1 + rng.bounded(5);  // 桩 1~5(避开故障的第 6 台)
                const double power = pilePower(pileIdx);
                // 时长按功率区分: 120kW 快充 10~55 分, 60kW 快充 15~100 分, 7kW 慢充 30~240 分
                const int durMin = power >= 100.0 ? 10 + rng.bounded(46)
                                 : power >= 50.0  ? 15 + rng.bounded(86)
                                                  : 30 + rng.bounded(211);

                const int startMinute = pickStartMinute(rng, durMin);
                const QString startTs = ts(day.year(), day.month(), day.day(),
                                           startMinute / 60, startMinute % 60);

                // 小概率是"预约后取消"的订单(status=3, 无开始/结束/费用)
                const bool canceled = rng.generateDouble() < 0.05;

                double energy = 0.0, amount = 0.0;
                QString endTs;
                int status = OrderFinished;
                if (canceled) {
                    status = OrderCanceled;
                } else {
                    energy = round2(power * durMin / 60.0);       // 电量 = 功率 × 时长
                    amount = round2(energy * price);              // 费用 = 电量 × 单价
                    endTs = QDateTime::fromString(startTs, QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                .addSecs(durMin * 60)
                                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
                }

                q.bindValue(QStringLiteral(":no"), QStringLiteral("T%1").arg(++orderSeq, 5, 10, QLatin1Char('0')));
                q.bindValue(QStringLiteral(":uid"), activeUserIds.at(userIdx));
                q.bindValue(QStringLiteral(":sid"), stationIds.at(si));
                q.bindValue(QStringLiteral(":cid"), allPiles.at(si).at(pileIdx - 1).chargerId);
                q.bindValue(QStringLiteral(":st"), status);
                q.bindValue(QStringLiteral(":en"), energy);
                q.bindValue(QStringLiteral(":am"), amount);
                q.bindValue(QStringLiteral(":sdt"), canceled ? QVariant() : QVariant(startTs));
                q.bindValue(QStringLiteral(":edt"), canceled ? QVariant() : QVariant(endTs));
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

    // 对应电桩置为"充电中"
    q.prepare(QStringLiteral("UPDATE charger SET status = :st WHERE charger_id = :id;"));
    q.bindValue(QStringLiteral(":st"), ChargerCharging);
    q.bindValue(QStringLiteral(":id"), activePile.chargerId);
    if (!q.exec()) { qDebug() << "种子数据: 更新电桩状态失败" << q.lastError().text(); dbc.rollback(); return false; }

    if (!dbc.commit()) {
        qDebug() << "种子数据: 提交事务失败";
        dbc.rollback();
        return false;
    }
    qDebug() << "种子数据完成:" << kUserCount << "用户 /" << kStationCount
             << "电站 /" << kStationCount * 6 << "桩 / 历史订单" << kHistoryDays
             << "天, 订单号到 T" << orderSeq;
    return true;
}

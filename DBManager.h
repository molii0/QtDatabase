#ifndef DBMANAGER_H
#define DBMANAGER_H

// ============================================================================
// DBManager —— 数据库管理类(实现按业务拆到多个 .cpp)
// ----------------------------------------------------------------------------
// 文件分工:
//   DBManager.h          接口(全部方法声明 + 表结构体)
//   DBManager.cpp        连接管理 / 建表 / 索引 / 版本迁移 / 初始化
//   DBManager_user.cpp   用户 + 管理员
//   DBManager_station.cpp 充电站 + 充电桩(增删改/批量编号/故障恢复)
//   DBManager_order.cpp  订单流程(orderConnect/orderStart/orderCancel/orderFinish)
//   DBManager_stats.cpp  统计(营收汇总/按日营收/电桩状态分布)
//   DBManager_seed.cpp   首次建库的演示数据
//   ChargeState.h/.cpp   状态机: 充电桩/订单的状态枚举与合法转换校验
//
// 状态约定(与 ChargeState.h 一致):
//   充电桩 status: 0空闲 1已连接 2充电中 3故障
//   订单   status: 0待支付 1充电中 2已完成 3已取消
//
// 线程安全说明:
//   Qt 要求"一个 QSqlDatabase 连接只能在其创建线程里使用", 所以本类给
//   "每个线程"各建一条独立连接(db())。两个客户端进程同时访问同一文件,
//   靠 WAL + busy_timeout + 外键约束处理, 不会报 "database is locked"。
// ============================================================================

#include "ChargeState.h"

#include <QMutex>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

class DBManager
{
public:
    // ---------- 与 user 表对应的结构 ----------
    struct User {
        qint64  userId = 0;
        QString phone;          // 手机号(登录凭证)
        QString nickname;
        double  balance = 0.0;  // 钱包余额(元)
        int     status = 1;     // 1=正常 0=冻结
        QString registerTime;   // 注册时间
    };

    // ---------- 与 station 表对应的结构 ----------
    struct Station {
        qint64  stationId = 0;
        QString name;
        QString codePrefix;     // 站点缩写(批量建桩编号用, 如 DR -> DR-01)
        QString address;
        double  longitude = 0.0;
        double  latitude = 0.0;
        double  price = 0.0;    // 充电单价(元/度)
    };

    // ---------- 与 charger 表对应的结构 ----------
    struct Charger {
        qint64  chargerId = 0;
        qint64  stationId = 0;
        QString code;           // 站内编号, 如 DR-01
        int     type = 0;       // 0=慢充 1=快充
        double  power = 0.0;    // 功率(kW)
        int     status = ChargerIdle;  // 见 ChargeState.h
    };

    // ---------- 与 charging_order 表对应的结构 ----------
    struct Order {
        qint64  orderId = 0;
        QString orderNo;        // 界面展示用的订单号
        qint64  userId = 0;
        qint64  stationId = 0;
        qint64  chargerId = 0;
        int     status = OrderWaitPay;  // 见 ChargeState.h
        double  energy = 0.0;   // 充电电量(度)
        double  amount = 0.0;   // 充电费用(元)
        QString startTime;      // 开始充电时间
        QString endTime;        // 结束时间
    };

    // 电桩状态分布统计(管理端用)
    struct ChargerStatusCount {
        qint64 idle = 0;        // 空闲
        qint64 connected = 0;   // 已连接
        qint64 charging = 0;    // 充电中
        qint64 fault = 0;       // 故障
        qint64 total = 0;
    };

    // 营收汇总(只统计已完成订单)
    struct RevenueSummary {
        double today = 0.0;
        double month = 0.0;
        double total = 0.0;
    };

    // 某一天的营收(管理端折线图用)
    struct RevenueDay {
        QString day;            // yyyy-MM-dd
        double amount = 0.0;
        qint64 orders = 0;
    };

    static DBManager& instance();

    // ---------------- 连接与初始化 ----------------
    // 打开数据库并建表/建索引; 新库自动写演示数据; 旧库自动执行增量迁移。
    bool init(const QString &dbFilePath = QString());
    bool isOpen() const;
    QString dbPath() const;
    QSqlDatabase db() const;            // 当前线程专用连接

    // 事务控制(当前线程连接上)
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    // ---------------- 用户(user) ----------------
    bool insertUser(const QString &phone, const QString &nickname);  // 注册; 手机号已存在返回 false
    bool getUserByPhone(const QString &phone, User *out = nullptr);  // 按手机号查, 返回是否存在
    bool getUserById(qint64 userId, User *out = nullptr) const;      // 按 id 查, 返回是否存在
    bool listUsers(const QString &phoneKeyword, QVector<User> *out) const;  // 管理端用户列表(支持模糊)
    bool setUserStatus(qint64 userId, bool frozen, QString *err = nullptr); // 冻结/解冻
    bool updateBalance(qint64 userId, double delta);                 // 余额增减(充值正数/扣费负数)

    // ---------------- 管理员(admin) ----------------
    bool checkAdminLogin(const QString &account, const QString &password);  // 种子默认 admin/123456

    // ---------------- 充电站(station) ----------------
    QVector<Station> listStations() const;                           // 全部电站
    bool getStation(qint64 stationId, Station *out = nullptr) const; // 查单个电站, 返回是否存在
    bool addStation(const QString &name, const QString &codePrefix, const QString &address,
                    double longitude, double latitude, double price,
                    qint64 *newStationId = nullptr, QString *err = nullptr);
    bool updateStation(qint64 stationId, const QString &name, const QString &codePrefix,
                       const QString &address, double longitude, double latitude,
                       double price, QString *err = nullptr);
    bool deleteStation(qint64 stationId, QString *err = nullptr);    // 有桩禁止删除(BR-10)
    bool stationHasChargers(qint64 stationId, bool *has = nullptr) const;

    // ---------------- 充电桩(charger) ----------------
    QVector<Charger> listChargers(qint64 stationId = -1) const;      // 某电站的电桩; -1=全部
    bool getCharger(qint64 chargerId, Charger *out = nullptr) const; // 查单个电桩, 返回是否存在
    bool addCharger(qint64 stationId, const QString &code, int type, double power,
                    QString *err = nullptr);
    bool batchAddChargers(qint64 stationId, int count, int type, double power,
                          int *created = nullptr, QString *err = nullptr);
    bool deleteCharger(qint64 chargerId, QString *err = nullptr);    // 有进行中订单/有历史订单禁止删除
    bool adminMarkFault(qint64 chargerId, QString *err = nullptr);   // 标记故障(仅空闲可标)
    bool adminRecover(qint64 chargerId, QString *err = nullptr);     // 故障 -> 空闲(远程重启/恢复)

    // ---------------- 订单流程(charging_order) ----------------
    // 1. 选桩下单: 校验用户/电桩后, 插入一条"待支付"订单, 电桩置"已连接"
    bool orderConnect(qint64 userId, qint64 chargerId,
                      qint64 *newOrderId = nullptr, QString *err = nullptr);
    // 2. 开始充电: 订单 待支付 -> 充电中, 电桩 已连接 -> 充电中(写 start_time)
    bool orderStart(qint64 orderId, QString *err = nullptr);
    // 3. 取消(未开始的订单): 待支付 -> 已取消, 电桩 已连接 -> 空闲
    bool orderCancel(qint64 orderId, QString *err = nullptr);
    // 4. 结束充电并结算(事务): 充电中 -> 已完成, 扣余额, 电桩 -> 空闲
    bool orderFinish(qint64 orderId, QString *err = nullptr);

    QVector<Order> listOrders(qint64 userId = -1) const;             // 订单倒序; -1=全部
    bool getOrderById(qint64 orderId, Order *out = nullptr) const;   // 查一条订单

    // ---------------- 统计(管理端) ----------------
    bool chargerStatusCount(ChargerStatusCount *out, QString *err = nullptr) const;
    bool revenueSummary(RevenueSummary *out, QString *err = nullptr) const;
    bool dailyRevenue(int days, QVector<RevenueDay> *out, QString *err = nullptr) const;

private:
    DBManager();
    ~DBManager();
    DBManager(const DBManager&) = delete;
    DBManager& operator=(const DBManager&) = delete;

    bool createTables();        // 建全部表 + schema_version(IF NOT EXISTS)
    bool createIndexes();       // 检索索引(IF NOT EXISTS, 旧库补建)
    // 版本迁移: 把数据库从 fromVersion 依次升级到最新版本(kSchemaVersion)
    bool applyMigrations(int fromVersion, QString *err);
    bool readSchemaVersion(int *version) const;
    bool writeSchemaVersion(int version);
    bool seedData();            // 首次演示数据(DBManager_seed.cpp)

    QString makeOrderNo();      // 生成唯一订单号
    static QString nowStr();    // 当前本地时间 "yyyy-MM-dd HH:mm:ss"
    static double round2(double v);   // 金额/电量保留 2 位小数

    static constexpr int kSchemaVersion = 2;    // 当前数据库结构版本

    QString m_dbPath;
    mutable QMutex m_openMutex; // 保护"每个线程首次建连接"的并发
};

#endif // DBMANAGER_H

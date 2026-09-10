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
//   DBManager_price.cpp  分时电价(峰谷平): 时段划分 + 按分钟切段的加权平均单价
//   DBManager_prediction.cpp 智能预测: 未来时段预测读取 + 演示用未来预测补充工具
//   DBManager_seed.cpp   首次建库的演示数据(底数)
//   DBManager_demogen.cpp 演示历史数据生成器(工具: 按需向前补历史订单/充值/
//                        运维日志/负荷预测; 供 Web 开发期一键造大量数据)
//   DBManager_device.cpp 设备接入(遥测/心跳/命令通道, Charger Simulator 对接)
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

#include <QDateTime>
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
        QString avatar;         // 头像(相对路径/文件名, 空串=默认灰头像)
        double  balance = 0.0;  // 钱包余额(元)
        double  debt = 0.0;     // 未结清欠费(元, >0 时禁止下单充电)
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
        double  price = 0.0;        // 平段价(元/度)
        double  pricePeak = 0.0;    // 峰段价(元/度, 分时电价)
        double  priceValley = 0.0;  // 谷段价(元/度, 分时电价)
    };

    // ---------- 分时电价(峰谷平) ----------
    // 时段划分(按充电发生的"分钟"切段计价): 峰 08-12、17-21; 谷 23-07; 其余为平
    enum PriceBand { BandPeak = 0, BandFlat = 1, BandValley = 2 };
    // minuteOfDay: 当天第几分钟(0..1439), 返回所在时段
    static PriceBand priceBandOfMinute(int minuteOfDay);
    // 一段充电(功率恒定)的加权平均单价(元/度): 从 start 起 durationSecs 秒,
    // 每个整分钟按所在时段取 ratePeak/rateFlat/rateValley 对应价, 首尾不足 1 分钟按比例加权。
    // 用法: amount = round2(energy * avgPriceOf(...)) —— 分时电价下仍与"电量×单价"一致。
    static double avgPriceOf(double ratePeak, double rateFlat, double rateValley,
                             const QDateTime &start, qint64 durationSecs);

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
        double  paid = 0.0;     // 实际扣款(元) = min(amount, 当时余额)
        double  debt = 0.0;     // 欠费金额(元, BR-06): amount - paid
        QString startTime;      // 开始充电时间
        QString endTime;        // 结束时间
        // 下面是查询时 JOIN 出来的展示字段(小票/列表用), 非表字段
        QString stationName;
        QString chargerCode;
        QString userPhone;
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

    // 按电站/按电桩的营收排行(管理端用)
    struct RevenueByItem {
        qint64  id = 0;         // station_id 或 charger_id
        QString name;           // 电站名 或 电桩编号
        double  amount = 0.0;
        qint64  orders = 0;
    };

    // 运维日志(ops_log 表)
    struct OpsLog {
        qint64  logId = 0;
        QString adminAccount;
        qint64  chargerId = 0;
        QString chargerCode;
        QString action;         // 如 标记故障 / 远程重启 / 恢复正常
        QString detail;
        QString createdAt;
    };

    // ---------- 设备遥测帧(与 charger_telemetry 表对应, 设备层每秒上报) ----------
    struct Telemetry {
        qint64  telemetryId = 0;
        qint64  chargerId = 0;
        QString ts;             // 设备上报时间
        QString status;         // 设备侧原始状态: idle/reserved/charging/finished/fault/offline
        double  power = 0.0;    // 当前输出功率 kW
        double  soc = 0.0;      // 电池电量 %
        double  energy = 0.0;   // 本次充电累计电量 kWh
        double  temperature = 0.0; // 设备温度 ℃
    };

    // ---------- 设备心跳(charger_heartbeat 表, 每桩一行) ----------
    struct Heartbeat {
        qint64  chargerId = 0;
        QString lastSeen;       // 最后一次心跳时间
        QString status;         // 心跳时的设备状态
        qint64  uptimeS = 0;    // 设备已运行秒数
    };

    // ---------- 设备命令(device_command 表, Server->Device 通道) ----------
    struct DeviceCommand {
        qint64  commandId = 0;
        qint64  chargerId = 0;
        QString command;        // plug/start/stop/unplug/fault/recover/offline/online/restart
        QString arg;            // 参数(如 fault 的故障码)
        int     status = 0;     // 0待执行 1已执行 2执行失败
        QString result;         // 设备回填的执行结果
        QString createdAt;
        QString doneAt;
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
    bool updateBalance(qint64 userId, double delta);                 // 余额增减(内部/通用原语)
    // 充值(推荐接口): 先还清欠费(debt), 剩余金额进入余额; 返回本次还款/剩余欠费
    bool recharge(qint64 userId, double amount,
                  double *repayOut = nullptr, double *remainDebtOut = nullptr,
                  QString *err = nullptr);
    bool updateNickname(qint64 userId, const QString &nickname, QString *err = nullptr); // 改昵称
    bool updateAvatar(qint64 userId, const QString &avatar, QString *err = nullptr);     // 设置头像(相对路径)

    // ---------------- 管理员(admin) ----------------
    bool checkAdminLogin(const QString &account, const QString &password);  // 种子默认 admin/123456

    // ---------------- 充电站(station) ----------------
    QVector<Station> listStations() const;                           // 全部电站
    bool getStation(qint64 stationId, Station *out = nullptr) const; // 查单个电站, 返回是否存在
    bool addStation(const QString &name, const QString &codePrefix, const QString &address,
                    double longitude, double latitude, double price,
                    qint64 *newStationId = nullptr, QString *err = nullptr,
                    double pricePeak = -1.0, double priceValley = -1.0);
    bool updateStation(qint64 stationId, const QString &name, const QString &codePrefix,
                       const QString &address, double longitude, double latitude,
                       double price, QString *err = nullptr,
                       double pricePeak = -1.0, double priceValley = -1.0);
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

    QVector<Order> listOrders(qint64 userId = -1) const;             // 订单倒序; -1=全部(含站名/桩号)
    bool getOrderById(qint64 orderId, Order *out = nullptr) const;   // 查一条订单(含站名/桩号)
    // 当前用户的"未结算订单"(待支付0/充电中1); 有则回填 out 并返回 true
    bool getActiveOrderOfUser(qint64 userId, Order *out = nullptr, QString *err = nullptr) const;

    // ---------------- 统计(管理端) ----------------
    bool chargerStatusCount(ChargerStatusCount *out, QString *err = nullptr) const;
    bool revenueSummary(RevenueSummary *out, QString *err = nullptr) const;
    bool dailyRevenue(int days, QVector<RevenueDay> *out, QString *err = nullptr) const;
    // 近 days 天按电站 / 按电桩的营收排行(按金额降序)
    bool revenueByStation(int days, QVector<RevenueByItem> *out, QString *err = nullptr) const;
    bool revenueByCharger(int days, QVector<RevenueByItem> *out, QString *err = nullptr) const;

    // ---------------- 运维日志(ops_log, UC-A-05) ----------------
    bool addOpsLog(const QString &adminAccount, qint64 chargerId, const QString &chargerCode,
                   const QString &action, const QString &detail = QString());
    bool listOpsLogs(int limit, QVector<OpsLog> *out, QString *err = nullptr) const;

    // ---------------- 设备接入(Charger Simulator 经数据库层对接) ----------------
    // 设备层的数据全部入库; 接口/界面等其他层一律经由数据库层读取, 不与设备直连。
    // 遥测: 每帧追加写入(旧帧用 trimTelemetry 按桩裁剪)
    bool insertTelemetry(const Telemetry &t, QString *err = nullptr);
    bool trimTelemetry(qint64 chargerId, int keepRows, QString *err = nullptr);
    // 心跳: 每桩一行 upsert; 平台按 now - last_seen 超过阈值(建议 15 秒)判定离线
    bool upsertHeartbeat(const Heartbeat &h, QString *err = nullptr);
    bool getHeartbeat(qint64 chargerId, Heartbeat *out = nullptr) const;
    bool listHeartbeats(QVector<Heartbeat> *out, QString *err = nullptr) const;
    // 设备状态投影到 charger.status(0空闲1已连接2充电中3故障):
    // 该桩存在进行中订单(0/1)时跳过不写, 状态归订单流程负责
    bool syncChargerDeviceStatus(qint64 chargerId, int status, QString *err = nullptr);
    // 命令通道: 平台 pushDeviceCommand 下发; 设备 takePendingDeviceCommands 领取,
    // 执行后 finishDeviceCommand 回填结果
    bool pushDeviceCommand(qint64 chargerId, const QString &command, const QString &arg = QString(),
                           qint64 *newCommandId = nullptr, QString *err = nullptr);
    bool takePendingDeviceCommands(qint64 chargerId, QVector<DeviceCommand> *out,
                                   QString *err = nullptr) const;
    bool finishDeviceCommand(qint64 commandId, bool ok, const QString &result,
                             QString *err = nullptr);
    bool listDeviceCommands(int limit, QVector<DeviceCommand> *out, QString *err = nullptr) const;

    // ---------------- 智能预测(load_prediction, 管理端"智能预测"页) ----------------
    // 曲线点: 某时刻的预测负荷(多站聚合或单站)
    struct PredictionPoint {
        QString targetTime;     // 预测目标时刻 yyyy-MM-dd HH:mm:ss
        double  loadKwh = 0.0;  // 该时刻预测电量(度)
        qint64  idleCount = 0;  // 该时刻预计空闲桩数(聚合=各站相加)
        int     isPeak = 0;     // 1=预测高峰时段
        qint64  stations = 0;   // 参与聚合的电站数
    };
    // 各站未来窗口的预测电量(降序)
    struct PredictionByStation {
        qint64  stationId = 0;
        QString name;
        double  loadKwh = 0.0;  // 窗口内预测总电量(度)
        qint64  points = 0;     // 预测点数(时段数)
        double  avgIdle = 0.0;  // 平均预计空闲桩数
    };
    // 读"未来 hours 小时"的预测: curve=逐时曲线, byStation=各站预测电量
    // (stationId <= 0 表示全部电站; lastGenerated 回填最近一次预测生成时间, 可为空)
    bool listPredictions(int hours, qint64 stationId,
                         QVector<PredictionPoint> *curve,
                         QVector<PredictionByStation> *byStation,
                         QString *lastGenerated = nullptr,
                         QString *err = nullptr) const;
    // 工具(演示/离线): 保证未来 hours 小时的逐时预测存在, 缺哪段补哪段(幂等);
    // 真实系统里 load_prediction 由 ML 预测脚本写入, 这里给演示库打底。
    bool ensureFuturePredictions(int hours = 24, qint64 *insertedOut = nullptr,
                                 QString *err = nullptr);

    // ---------------- 演示历史数据生成器(工具, 非业务功能) ----------------
    // 给 Web/图表演示造大量"历史数据"的离线工具, 不属于业务增删改:
    // 业务数据增删改一律走 REST 接口, 本生成器只批量回填"过去的历史",
    // 与正式业务流程分离。详见 DBManager_demogen.cpp。
    struct DemoGenResult {
        int    daysRequested = 0;   // 本次要求覆盖的历史天数
        int    daysGenerated = 0;   // 本次实际补出的天数(0=已覆盖, 无需生成)
        qint64 ordersAdded    = 0;  // 新增历史订单数
        qint64 rechargesAdded = 0;  // 新增充值流水数
        qint64 opsLogsAdded   = 0;  // 新增运维日志数
        qint64 predictionsAdded = 0;// 新增负荷预测行数
    };
    // 让 charging_order 覆盖到最近 days 天: 已有部分不动, 只向前补更早的缺失日期
    // (幂等, 可重复运行); 同一窗口顺手补齐 recharge_log / ops_log / load_prediction。
    // density: 订单/充值/运维日志的密度倍率(默认 1.0, 建议 0.1~10;
    // 负荷预测固定每站每天 6 行, 不受 density 影响)。
    bool generateDemoHistory(int days, double density = 1.0,
                             DemoGenResult *out = nullptr, QString *err = nullptr);

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

    static constexpr int kSchemaVersion = 8;    // 当前数据库结构版本
    // v5: 订单 paid/debt; v6: user.debt(未结清欠费)+充值先还款+欠费禁充(BR-04/BR-06 闭环)
    // v7: 设备接入表 charger_telemetry/charger_heartbeat/device_command(模拟器对接)
    // v8: 分时电价 station.price_peak/price_valley(峰谷平)

    QString m_dbPath;
    mutable QMutex m_openMutex; // 保护"每个线程首次建连接"的并发
};

#endif // DBMANAGER_H

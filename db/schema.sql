-- ============================================================================
-- 数据库模式(schema) —— 当前最新结构的完整建表脚本
-- 运行时由 DBManager::createTables() 从 Qt 资源 ":/db/schema.sql" 读取并执行,
-- 全部使用 IF NOT EXISTS(幂等)。本文件同时可作人工查阅/评审用。
-- ============================================================================

-- 结构版本表(记录当前结构版本号, 启动时按版本补迁移)
CREATE TABLE IF NOT EXISTS schema_version (
    version    INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL DEFAULT (datetime('now', 'localtime'))
);

-- 用户表
CREATE TABLE IF NOT EXISTS user (
    user_id       INTEGER PRIMARY KEY AUTOINCREMENT,
    phone         TEXT    NOT NULL UNIQUE,
    nickname      TEXT    NOT NULL,
    avatar        TEXT    NOT NULL DEFAULT '',
    balance       REAL    NOT NULL DEFAULT 0,
    status        INTEGER NOT NULL DEFAULT 1,
    register_time TEXT    NOT NULL DEFAULT (datetime('now', 'localtime')),
    debt          REAL    NOT NULL DEFAULT 0   -- 未结清欠费(元, BR-06/欠费禁充)
);

-- 管理员表
CREATE TABLE IF NOT EXISTS admin (
    admin_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    account    TEXT NOT NULL UNIQUE,
    password   TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (datetime('now', 'localtime'))
);

-- 充电站表(code_prefix 是站点缩写, 批量建桩编号用)
CREATE TABLE IF NOT EXISTS station (
    station_id  INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL,
    code_prefix TEXT NOT NULL DEFAULT '',
    address     TEXT NOT NULL DEFAULT '',
    longitude   REAL NOT NULL,
    latitude    REAL NOT NULL,
    price       REAL NOT NULL DEFAULT 0
);

-- 充电桩表(status 见 ChargeState.h: 0空闲 1已连接 2充电中 3故障)
CREATE TABLE IF NOT EXISTS charger (
    charger_id INTEGER PRIMARY KEY AUTOINCREMENT,
    station_id INTEGER NOT NULL REFERENCES station(station_id),
    code       TEXT    NOT NULL,
    type       INTEGER NOT NULL DEFAULT 0,
    power      REAL    NOT NULL,
    status     INTEGER NOT NULL DEFAULT 0,
    UNIQUE (station_id, code)
);

-- 订单表(status 见 ChargeState.h: 0待支付 1充电中 2已完成 3已取消)
CREATE TABLE IF NOT EXISTS charging_order (
    order_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    order_no   TEXT NOT NULL UNIQUE,
    user_id    INTEGER NOT NULL REFERENCES user(user_id),
    station_id INTEGER NOT NULL REFERENCES station(station_id),
    charger_id INTEGER NOT NULL REFERENCES charger(charger_id),
    status     INTEGER NOT NULL DEFAULT 0,
    energy     REAL    NOT NULL DEFAULT 0,
    amount     REAL    NOT NULL DEFAULT 0,
    paid       REAL    NOT NULL DEFAULT 0,
    debt       REAL    NOT NULL DEFAULT 0,
    start_time TEXT,
    end_time   TEXT,
    created_at TEXT    NOT NULL DEFAULT (datetime('now', 'localtime'))
);

-- 运维日志表(UC-A-05: 远程重启/标记故障/恢复正常等记录)
CREATE TABLE IF NOT EXISTS ops_log (
    log_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    admin_account TEXT NOT NULL DEFAULT '',
    charger_id    INTEGER REFERENCES charger(charger_id),
    charger_code  TEXT NOT NULL DEFAULT '',
    action        TEXT NOT NULL,
    detail        TEXT NOT NULL DEFAULT '',
    created_at    TEXT NOT NULL DEFAULT (datetime('now', 'localtime'))
);

-- 充值流水表(UC-U-05: 每次充值记一条)
CREATE TABLE IF NOT EXISTS recharge_log (
    recharge_id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL REFERENCES user(user_id),
    amount      REAL NOT NULL,
    created_at  TEXT NOT NULL DEFAULT (datetime('now', 'localtime'))
);

-- 负荷预测表(UC-A-08/UC-M-03: 机器学习结果回写)
CREATE TABLE IF NOT EXISTS load_prediction (
    prediction_id INTEGER PRIMARY KEY AUTOINCREMENT,
    station_id    INTEGER NOT NULL REFERENCES station(station_id),
    generated_at  TEXT NOT NULL,
    target_time   TEXT NOT NULL,
    load_kwh      REAL NOT NULL DEFAULT 0,
    idle_count    INTEGER NOT NULL DEFAULT 0,
    is_peak       INTEGER NOT NULL DEFAULT 0
);

-- 设备遥测表(Charger Device Simulator 每秒上报一帧, 旧帧按桩裁剪)
CREATE TABLE IF NOT EXISTS charger_telemetry (
    telemetry_id INTEGER PRIMARY KEY AUTOINCREMENT,
    charger_id   INTEGER NOT NULL REFERENCES charger(charger_id),
    ts           TEXT NOT NULL,
    status       TEXT NOT NULL,
    power        REAL NOT NULL DEFAULT 0,
    soc          REAL NOT NULL DEFAULT 0,
    energy       REAL NOT NULL DEFAULT 0,
    temperature  REAL NOT NULL DEFAULT 0
);

-- 设备心跳表(每桩一行, 上报即刷新; 超过阈值未刷新视为离线)
CREATE TABLE IF NOT EXISTS charger_heartbeat (
    charger_id INTEGER PRIMARY KEY REFERENCES charger(charger_id),
    last_seen  TEXT NOT NULL,
    status     TEXT NOT NULL DEFAULT '',
    uptime_s   INTEGER NOT NULL DEFAULT 0
);

-- 设备命令表(Server -> Device 通道: 平台写入, 设备轮询执行后回填)
CREATE TABLE IF NOT EXISTS device_command (
    command_id INTEGER PRIMARY KEY AUTOINCREMENT,
    charger_id INTEGER NOT NULL REFERENCES charger(charger_id),
    command    TEXT NOT NULL,
    arg        TEXT NOT NULL DEFAULT '',
    status     INTEGER NOT NULL DEFAULT 0,
    result     TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT (datetime('now', 'localtime')),
    done_at    TEXT
);

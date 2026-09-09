-- ============================================================================
-- 迁移 7: 设备接入表 —— Charger Device Simulator(设备层)与数据库层对接
--   设备层只与数据库层对接, 数据直接入库; 其他层(接口/界面)一律经由数据库层。
--   a) charger_telemetry  设备遥测帧(每秒一帧, 追加写入, 旧帧按桩裁剪)
--   b) charger_heartbeat  设备心跳(每桩一行, 上报即刷新 last_seen;
--                         平台按 now - last_seen 超过阈值判定设备离线)
--   c) device_command     Server->Device 命令通道(平台写入, 设备轮询执行后回填)
-- 注意: 注释行内不要出现 ASCII 分号(会干扰按分号切分执行)
-- ============================================================================
CREATE TABLE IF NOT EXISTS charger_telemetry (
    telemetry_id INTEGER PRIMARY KEY AUTOINCREMENT,
    charger_id   INTEGER NOT NULL REFERENCES charger(charger_id),
    ts           TEXT NOT NULL,                -- 设备上报时间
    status       TEXT NOT NULL,                -- 设备侧原始状态(设备状态机词汇)
    power        REAL NOT NULL DEFAULT 0,      -- 当前输出功率 kW
    soc          REAL NOT NULL DEFAULT 0,      -- 电池电量 %
    energy       REAL NOT NULL DEFAULT 0,      -- 本次充电累计电量 kWh
    temperature  REAL NOT NULL DEFAULT 0       -- 设备温度 ℃
);

CREATE TABLE IF NOT EXISTS charger_heartbeat (
    charger_id INTEGER PRIMARY KEY REFERENCES charger(charger_id),
    last_seen  TEXT NOT NULL,                  -- 最后一次心跳时间
    status     TEXT NOT NULL DEFAULT '',       -- 心跳时的设备状态
    uptime_s   INTEGER NOT NULL DEFAULT 0      -- 设备已运行秒数
);

CREATE TABLE IF NOT EXISTS device_command (
    command_id INTEGER PRIMARY KEY AUTOINCREMENT,
    charger_id INTEGER NOT NULL REFERENCES charger(charger_id),
    command    TEXT NOT NULL,                  -- plug/start/stop/unplug/fault/recover/offline/online/restart
    arg        TEXT NOT NULL DEFAULT '',       -- 命令参数(如 fault 的故障码)
    status     INTEGER NOT NULL DEFAULT 0,     -- 0待执行 1已执行 2执行失败
    result     TEXT NOT NULL DEFAULT '',       -- 设备回填的执行结果
    created_at TEXT NOT NULL DEFAULT (datetime('now', 'localtime')),
    done_at    TEXT                            -- 设备执行完成时间
);

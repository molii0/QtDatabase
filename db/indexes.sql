-- ============================================================================
-- 检索索引 + 业务约束索引(全部幂等, 每次启动在"迁移之后"执行一次)
-- ============================================================================

-- 电站下的电桩列表查询: WHERE station_id = ?
CREATE INDEX IF NOT EXISTS idx_charger_station ON charger (station_id);

-- 电桩按状态过滤(电站列表"空闲数"统计)
CREATE INDEX IF NOT EXISTS idx_charger_status ON charger (status);

-- 某用户的订单历史: WHERE user_id = ?
CREATE INDEX IF NOT EXISTS idx_order_user ON charging_order (user_id);

-- 查"某用户未结算订单"(待支付0/充电中1) —— 联合索引
CREATE INDEX IF NOT EXISTS idx_order_user_status ON charging_order (user_id, status);

-- 按订单状态筛选(管理端订单页)
CREATE INDEX IF NOT EXISTS idx_order_status ON charging_order (status);

-- 某电桩是否被订单占用 / 电桩使用记录
CREATE INDEX IF NOT EXISTS idx_order_charger ON charging_order (charger_id);

-- BR-02: 同一用户同时最多 1 条未结算订单(数据库级强制)
CREATE UNIQUE INDEX IF NOT EXISTS uq_order_user_active
    ON charging_order (user_id) WHERE status IN (0, 1);

-- BR-03: 同一电桩同时最多被 1 条订单占用(数据库级强制)
CREATE UNIQUE INDEX IF NOT EXISTS uq_order_charger_active
    ON charging_order (charger_id) WHERE status IN (0, 1);

-- 新表配套索引
CREATE INDEX IF NOT EXISTS idx_recharge_user ON recharge_log (user_id);
CREATE INDEX IF NOT EXISTS idx_pred_station ON load_prediction (station_id);

-- 设备接入(遥测按桩取最新若干帧 / 待执行命令按桩领取)
CREATE INDEX IF NOT EXISTS idx_telemetry_charger ON charger_telemetry (charger_id, telemetry_id);
CREATE INDEX IF NOT EXISTS idx_dcmd_charger_status ON device_command (charger_id, status);

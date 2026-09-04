-- ============================================================================
-- 迁移 3: 加 BR-02/03 唯一索引前的数据清理
-- 旧版没有约束, 可能存在"同一用户/同一电桩有多条活动订单(待支付0/充电中1)"。
-- 只保留每人/每桩最新的一条活动订单, 其余置为已取消, 并释放不再被占用的电桩。
-- ============================================================================

-- 同一用户保留最新一条活动订单, 其余置已取消
UPDATE charging_order SET status = 3 WHERE order_id IN (
  SELECT o.order_id FROM charging_order o
   WHERE o.status IN (0, 1) AND EXISTS (
     SELECT 1 FROM charging_order o2
      WHERE o2.user_id = o.user_id AND o2.status IN (0, 1) AND o2.order_id > o.order_id));

-- 同一电桩保留最新一条活动订单, 其余置已取消
UPDATE charging_order SET status = 3 WHERE order_id IN (
  SELECT o.order_id FROM charging_order o
   WHERE o.status IN (0, 1) AND EXISTS (
     SELECT 1 FROM charging_order o2
      WHERE o2.charger_id = o.charger_id AND o2.status IN (0, 1) AND o2.order_id > o.order_id));

-- 释放那些已经没有任何活动订单、但仍显示占用中的电桩
UPDATE charger SET status = 0 WHERE charger_id NOT IN (
    SELECT charger_id FROM charging_order WHERE status IN (0, 1))
    AND status IN (1, 2);

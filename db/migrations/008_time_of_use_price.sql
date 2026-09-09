-- ============================================================================
-- 迁移 8: 分时电价(峰谷平)
--   原 station.price 视为"平段价", 补两列: price_peak(峰段)/price_valley(谷段)
--   时段划分(按充电发生分钟切段计价): 峰 08-12、17-21; 谷 23-07; 其余为平
-- 说明: 已有电站按 峰=平×1.35、谷=平×0.55 回填; 想换更低/更高的价,
--   重新生成演示库或在管理端修改即可。注释行内不要出现 ASCII 分号
-- ============================================================================
ALTER TABLE station ADD COLUMN price_peak REAL NOT NULL DEFAULT 0;
ALTER TABLE station ADD COLUMN price_valley REAL NOT NULL DEFAULT 0;

UPDATE station
   SET price_peak   = ROUND(price * 1.35, 2),
       price_valley = ROUND(price * 0.55, 2);

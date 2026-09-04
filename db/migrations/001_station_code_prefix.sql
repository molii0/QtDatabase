-- ============================================================================
-- 迁移 1: station 表补 code_prefix 字段, 并从已有桩号(如 DR-01)回填前缀
-- 注意: 本文件只在该列确实不存在时执行(DBManager 里先查 PRAGMA table_info 判断),
-- 列已存在(早期版本手工补过)的库会跳过本文件但同样登记版本 1。
-- ============================================================================
ALTER TABLE station ADD COLUMN code_prefix TEXT NOT NULL DEFAULT '';

UPDATE station SET code_prefix = COALESCE(
  (SELECT substr(c.code, 1, instr(c.code, '-') - 1) FROM charger c
    WHERE c.station_id = station.station_id AND instr(c.code, '-') > 0 LIMIT 1), '')
WHERE code_prefix = '';

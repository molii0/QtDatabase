-- ============================================================================
-- 迁移 5: charging_order 补 paid / debt 两列(BR-06 欠费结算)
--   paid: 实际扣款金额(元) = min(amount, 结算时余额)
--   debt: 欠费金额(元)     = amount - paid
-- 旧已完成订单两列填 0(当时均全额扣款)
-- ============================================================================
ALTER TABLE charging_order ADD COLUMN paid REAL NOT NULL DEFAULT 0;
ALTER TABLE charging_order ADD COLUMN debt REAL NOT NULL DEFAULT 0;

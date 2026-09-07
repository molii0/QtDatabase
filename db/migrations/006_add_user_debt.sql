-- ============================================================================
-- 迁移 6: user 表补 debt(未结清欠费) —— 欠费禁充(BR-04/BR-06 闭环)
--   user.debt 表示「未结清的欠费总额」
--   a) 把已有数据库里订单级欠费一次性并入 user.debt(此前没有还款机制)
--   b) 之后的行为: 结算不足会把差额累加进 user.debt
--      充值会先还 user.debt, 多余部分才进入 balance
-- 注意: 注释行内不要出现 ASCII 分号(会干扰按分号切分执行)
-- ============================================================================
ALTER TABLE user ADD COLUMN debt REAL NOT NULL DEFAULT 0;

UPDATE user SET debt = COALESCE(
    (SELECT ROUND(SUM(debt), 2) FROM charging_order
      WHERE charging_order.user_id = user.user_id AND debt > 0), 0);

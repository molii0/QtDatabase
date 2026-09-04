#include "ChargeState.h"

// ============================================================================
// 状态机实现(合法的状态转换表 + 校验函数)
// ============================================================================

QString chargerStateText(int state)
{
    switch (state) {
    case ChargerIdle:      return QStringLiteral("空闲");
    case ChargerConnected: return QStringLiteral("已连接");
    case ChargerCharging:  return QStringLiteral("充电中");
    case ChargerFault:     return QStringLiteral("故障");
    default:               return QStringLiteral("未知(%1)").arg(state);
    }
}

QString orderStateText(int state)
{
    switch (state) {
    case OrderWaitPay:   return QStringLiteral("待支付");
    case OrderCharging:  return QStringLiteral("充电中");
    case OrderFinished:  return QStringLiteral("已完成");
    case OrderCanceled:  return QStringLiteral("已取消");
    default:             return QStringLiteral("未知(%1)").arg(state);
    }
}

bool chargerCanTransition(int from, int to, QString *why)
{
    // 允许的转换:
    //   空闲 -> 已连接   (用户下单选桩)
    //   空闲 -> 故障     (管理员标记故障)
    //   已连接 -> 充电中 (用户点"开始充电")
    //   已连接 -> 空闲   (取消未开始的订单/放弃选桩)
    //   充电中 -> 空闲   (结算完成/释放)
    //   故障   -> 空闲   (管理员远程重启/恢复正常)
    const bool allowed = (from == ChargerIdle && to == ChargerConnected)
                      || (from == ChargerIdle && to == ChargerFault)
                      || (from == ChargerConnected && to == ChargerCharging)
                      || (from == ChargerConnected && to == ChargerIdle)
                      || (from == ChargerCharging && to == ChargerIdle)
                      || (from == ChargerFault && to == ChargerIdle);
    if (!allowed && why)
        *why = QStringLiteral("电桩状态不允许从[%1]变为[%2]")
                   .arg(chargerStateText(from), chargerStateText(to));
    return allowed;
}

bool orderCanTransition(int from, int to, QString *why)
{
    // 允许的转换:
    //   待支付 -> 充电中 (开始充电)
    //   待支付 -> 已取消 (取消未开始的订单)
    //   充电中 -> 已完成 (结算成功)
    const bool allowed = (from == OrderWaitPay && to == OrderCharging)
                      || (from == OrderWaitPay && to == OrderCanceled)
                      || (from == OrderCharging && to == OrderFinished);
    if (!allowed && why)
        *why = QStringLiteral("订单状态不允许从[%1]变为[%2]")
                   .arg(orderStateText(from), orderStateText(to));
    return allowed;
}

#ifndef CHARGESTATE_H
#define CHARGESTATE_H

// ============================================================================
// 状态机定义 —— 充电桩 / 订单的"合法状态转换表"
// ----------------------------------------------------------------------------
// 所有会改状态的业务操作(开始充电/结束结算/取消/标记故障…)在执行数据库修改
// 之前, 都先查这张表: 转换合法才执行, 非法直接拒绝并说明原因(HTTP 层返回 409)。
// 这样可以防止网络重发、并发操作把状态改乱(学习到其他组的状态机做法)。
//
// 充电桩 charger.status:
//   0 = 空闲      (没人用)
//   1 = 已连接    (用户选桩下单, 枪已插好, 尚未开始计费)
//   2 = 充电中    (正在计费充电)
//   3 = 故障
//
// 订单 charging_order.status:
//   0 = 待支付    (订单刚创建/预约, 尚未开始充电)
//   1 = 充电中
//   2 = 已完成
//   3 = 已取消
// ============================================================================

#include <QString>

enum ChargerState {
    ChargerIdle = 0,       // 空闲
    ChargerConnected = 1,  // 已连接(已选桩, 待开始)
    ChargerCharging = 2,   // 充电中
    ChargerFault = 3       // 故障
};

enum OrderState {
    OrderWaitPay = 0,      // 待支付(预约/待开始)
    OrderCharging = 1,     // 充电中
    OrderFinished = 2,     // 已完成
    OrderCanceled = 3      // 已取消
};

// 状态的中文显示名(界面 / 接口返回都用它)
QString chargerStateText(int state);
QString orderStateText(int state);

// 电桩状态是否允许 from -> to; 不允许时 why 带回原因
bool chargerCanTransition(int from, int to, QString *why = nullptr);

// 订单状态是否允许 from -> to; 不允许时 why 带回原因
bool orderCanTransition(int from, int to, QString *why = nullptr);

#endif // CHARGESTATE_H

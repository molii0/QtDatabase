# REST 接口文档（前后端契约）

> 协议：HTTP/1.1 + JSON；服务器基于官方 **QHttpServer**（Qt 自带模块，`QT += httpserver`）。
> 启动：`QtDatabase.exe [数据库文件] --server [端口]`，默认 `http://127.0.0.1:8080`。
>
> - 所有接口请求/响应体都是 JSON；
> - 成功按 HTTP 语义返回资源 JSON；失败返回 `{"error": "原因"}`；
> - 状态码：200 成功、201 创建、400 参数错误、401 未登录/token 无效、403 被冻结、
>   404 资源不存在、409 业务冲突（状态不允许/被占用/余额不足等）、500 服务器错误。

## 状态说明（字段里的 status/statusText）

- 电桩 `status`：0 空闲 / 1 已连接 / 2 充电中 / 3 故障
- 订单 `status`：0 待支付 / 1 充电中 / 2 已完成 / 3 已取消
- 用户 `status`：1 正常 / 0 冻结

---

## 一、C 端（用户）

### 1. 手机号登录（不存在自动注册）
`POST /api/users/login`　body:`{"phone":"13900000001","nickname":"可选"}`
→ `200 {"user":{userId,phone,nickname,balance,status,statusText,registerTime}}`
错误：400 手机号格式错；403 账号已冻结。

### 2. 电站列表（含桩统计）
`GET /api/stations`
→ `200 {"stations":[{stationId,name,address,longitude,latitude,price,codePrefix,totalChargers,idleChargers,connectedChargers,chargingChargers,faultChargers}]}`

### 3. 电桩列表
`GET /api/chargers?stationId=1`（不带 stationId 返回全部）
→ `200 {"chargers":[{chargerId,stationId,code,type,typeText,power,status,statusText}]}`

### 4. 选桩下单（订单→待支付，电桩→已连接）
`POST /api/charges`　body:`{"userId":1,"chargerId":2}`
→ `201 {"order":{...}}`（orderId 用于后续 start/finish/cancel）
错误：409 电桩被占用/用户被冻结(403)。

### 5. 开始充电
`POST /api/charges/{orderId}/start`
→ `200 {order}`；错误：409（订单状态不是待支付等）。

### 6. 结束充电并结算
`POST /api/charges/{orderId}/finish`（**充电时长需 >0 秒**，结束太快要稍等）
→ `200 {order}`（status=2，含 energy/amount/endTime）
错误：409 余额不足/时长不足/状态不允许。

### 7. 取消订单（仅未开始的）
`DELETE /api/charges/{orderId}`
→ `200 {"message":"订单已取消"}`；错误：409（充电中不能取消、重复取消）。

### 8. 我的订单
`GET /api/users/{userId}/orders`
→ `200 {"orders":[{orderId,orderNo,status,statusText,energy,amount,startTime,endTime,...}]}`

### 9. 充值
`POST /api/users/{userId}/recharge`　body:`{"amount":100}`
→ `200 {"user":{...}}`（含最新余额）；错误：400 金额非法、404 用户不存在。

---

## 二、管理端（先登录拿 token）

管理端请求都要带请求头：`Authorization: Bearer <token>`

### 10. 管理员登录
`POST /api/admin/login`　body:`{"account":"admin","password":"123456"}`
→ `200 {"token":"...","account":"admin"}`；错误：401 账号或密码错误。

### 11. 退出登录
`POST /api/admin/logout`

### 12. 用户列表（模糊搜索）
`GET /api/admin/users?keyword=139`
→ `200 {"users":[...]}`

### 13. 冻结 / 解冻用户
`PUT /api/admin/users/{userId}/status`　body:`{"frozen":true}`
→ `200 {"userId":6,"frozen":true}`

### 14. 营收 + 电桩状态总览
`GET /api/admin/stats`
→ `200 {"revenue":{"today","month","total"},"charger":{"idle","connected","charging","fault","total"}}`

### 15. 近 N 天营收（折线图）
`GET /api/admin/stats/daily?days=7`
→ `200 {"daily":[{day,amount,orders}]}`（无数据日补 0）

### 16. 新增电站
`POST /api/admin/stations`　body:`{"name","codePrefix","address","longitude","latitude","price"}`
→ `201 {"stationId":6,"message":"新增充电站成功"}`

### 17. 修改电站
`PUT /api/admin/stations/{stationId}`　body 同上
→ `200 {"message":"修改充电站成功"}`

### 18. 删除电站
`DELETE /api/admin/stations/{stationId}`
错误：409 站下仍有电桩(BR-10)。

### 19. 批量建桩（自动编号 缩写-序号）
`POST /api/admin/stations/{stationId}/chargers/batch`　body:`{"count":5,"type":0,"power":7}`
→ `201 {"created":5,"message":"批量建桩成功"}`

### 20. 删除电桩
`DELETE /api/admin/chargers/{chargerId}`
错误：409 有进行中订单/已有历史订单。

### 21. 电桩故障 / 恢复（远程重启）
`PUT /api/admin/chargers/{chargerId}/action`　body:`{"action":"fault"|"recover"|"restart"}`
→ `200 {"chargerId","status","statusText"}`
注意：只能对“空闲”桩标记故障；恢复只能对“故障”桩。非法转换返回 409。
**每次成功都会自动写入一条运维日志(ops_log)，记录操作账号与动作。**

### 22. 管理端订单列表
`GET /api/admin/orders?userId=1&status=2`（参数都可选）
→ `200 {"orders":[...]}`

### 23. 运维日志列表
`GET /api/admin/logs?limit=50`（最近 limit 条，倒序）
→ `200 {"logs":[{logId,adminAccount,chargerId,chargerCode,action,detail,createdAt}]}`
来源：管理端对电桩的“标记故障/恢复/远程重启”等操作会自动落库。

---

## 三、一个完整充电流程示例

```
POST /api/users/login                     {"phone":"13900000001"}
GET  /api/chargers?stationId=1            → 挑一台 status=0 的桩 chargerId
POST /api/charges                          {"userId":..,"chargerId":..}  → orderId
POST /api/charges/{orderId}/start          → 开始充电(充电中)
…(客户端每秒定时器模拟计电、等几分钟)…
POST /api/charges/{orderId}/finish         → 结算，返回电量/费用/结束时间
```

## 四、错误码使用建议（前端）

- 400：检查参数；401：跳登录（管理端）；403：提示“账号已冻结”；
- 409：按 `error` 文案提示（如“余额不足，请先充值”“该电桩正在使用中”）；
- 其它：提示“服务异常，请稍后再试”。

## 五、已规划但未实现（按需再加）

设备心跳/告警、实时电压电流、预约超时释放、欠费处理、运维日志/充值流水/预测等扩展表、
微信支付/发票/收藏等（部分需要新表或真实外部依赖）。

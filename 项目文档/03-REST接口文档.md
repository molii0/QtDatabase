# REST 接口文档（前后端契约）

> 协议：HTTP/1.1 + JSON；服务器基于官方 **QHttpServer**（`QT += httpserver`）。
> 启动：`QtDatabase.exe [数据库文件] --server [端口]`，默认 `http://127.0.0.1:8080`。
>
> - 请求/响应体均为 JSON（UTF-8）；
> - 成功按 HTTP 语义返回资源 JSON；失败返回 `{"error": "原因"}`；
> - 状态码：200 成功、201 创建、400 参数错误、401 未登录/token 无效、403 被冻结或越权、
>   404 资源不存在、409 业务冲突（状态不允许/被占用/余额不足等）、500 服务器错误；
> - **鉴权**：用户端和管理端登录后都会返回 `token`；除公开查询接口外，请求需带请求头
>   `Authorization: Bearer <token>`；用他人 token 访问他人数据返回 403。
> - 金额单位元、电量度、距离公里（保留 1 位小数），金额/电量保留 2 位小数。

## 状态说明（字段里的 status/statusText）

- 电桩 `status`：0 空闲 / 1 已连接 / 2 充电中 / 3 故障
- 订单 `status`：0 待支付 / 1 充电中 / 2 已完成 / 3 已取消
- 用户 `status`：1 正常 / 0 冻结

## 公共数据对象（接口会反复用到）

| 对象 | 字段 |
| --- | --- |
| user | userId, phone, nickname, **avatar**, balance, **debt(未结清欠费)**, status, statusText, registerTime |
| station | stationId, name, codePrefix, address, longitude, latitude, **price(平段价), pricePeak(峰段价), priceValley(谷段价)**, totalChargers, idleChargers, connectedChargers, chargingChargers, faultChargers, **onlineRate**(%)，附近电站另有 **distanceKm** |
| charger | chargerId, stationId, code, type, typeText, power, status, statusText |
| order | orderId, orderNo, userId, userPhone, stationId, **stationName**, chargerId, **chargerCode**, status, statusText, energy, amount, **paid(实扣)**, **debt(欠费)**, startTime, endTime |

> **分时电价（峰谷平）**：`station.price` 是平段价(元/度，种子 0.65~0.92，已下调)；
> `pricePeak/priceValley` 是峰/谷段价。时段：峰 08-12、17-21；谷 23-07；其余为平。
> 订单 `amount` 按充电发生的**分钟切段**、用时段加权均价结算（`DBManager_price.cpp`），
> 所以同一度电深夜比高峰便宜。新建/修改电站可传可选字段 `pricePeak/priceValley`，
> 不传则按 平×1.35 / 平×0.55 自动推导（见 §21/22）。

---

## 一、C 端（用户）

> 公开（无需 token）：`2.电站列表`、`3.附近电站`、`4.电桩列表`；
> 其余操作都需要用户 token（见 1.登录返回）。

### 1. 手机号登录（不存在自动注册）
`POST /api/users/login`　body:`{"phone":"13900000001","nickname":"可选"}`
→ `200 {"token":"...","user":{user 对象}}`
错误：400 手机号格式错；403 账号已冻结。
> 之后 5~12 的请求都带 `Authorization: Bearer <该token>`。

### 2. 电站列表（含桩统计与在线率）
`GET /api/stations`
→ `200 {"stations":[station 对象...]}`

### 3. 附近电站（按距离升序）
`GET /api/stations/nearby?latitude=41.8&longitude=123.4`
→ `200 {"stations":[station 对象(含 distanceKm)...]}`（按距离由近到远）
错误：400 缺少/非法坐标。

### 4. 电桩列表
`GET /api/chargers?stationId=1`（不带 stationId 返回全部）
→ `200 {"chargers":[charger 对象...]}`

### 5. 选桩下单（订单→待支付，电桩→已连接）
`POST /api/charges`　body:`{"chargerId":2}`（userId 取自 token，可不传）
→ `201 {"order":order 对象}`
前置规则：**有未结清欠费(debt>0)禁止下单**；**余额需 ≥ 起充金额(默认 5 元)**(BR-04)。
错误：409 欠费禁充/余额不足(起充)/已有未结算订单(BR-02)/电桩被占用/故障；403 冻结。

### 6. 开始充电
`POST /api/charges/{orderId}/start`
→ `200 {order}`；错误：409 状态不允许；404 订单不存在；403 他人订单。

### 7. 结束充电并结算
`POST /api/charges/{orderId}/finish`（**充电时长需 >0 秒**，结束太快要稍等）
→ `200 {order}`（status=2，含 energy/amount/**paid/debt**/endTime/stationName/chargerCode）
结算规则(BR-06)：余额充足时 paid=amount、debt=0；**余额不足也能正常结束**——
把余额扣到 0，差额 amount-paid 记为该订单欠费 debt，并**累加到用户未结清欠费 user.debt**（欠费禁充用）。
错误：409 时长不足/状态不允许；404/403 同上（余额不足不再是错误）。

### 8. 取消订单（仅未开始的）
`DELETE /api/charges/{orderId}`
→ `200 {"message":"订单已取消"}`；错误：409（充电中不能取消、重复取消）。

### 9. 我的订单
`GET /api/users/{userId}/orders`
→ `200 {"orders":[order 对象...]}`（含站名/桩号/手机号，倒序）

### 10. 充值（先还欠费，剩余进余额）
`POST /api/users/{userId}/recharge`　body:`{"amount":100}`
→ `200 {"user":user 对象, "repayAmount":本次还款, "remainingDebt":剩余欠费}`
规则：有未结清欠费时，充值金额**先用于还款**，多余部分才进入余额；
还清欠费且余额 ≥ 起充金额(5 元)后才能再次下单。
错误：400 金额非法、404 用户不存在。

### 11. 修改资料（昵称/头像）
`PUT /api/users/{userId}/profile`　body:`{"nickname":"新昵称","avatar":"avatars/1.png"}`（都可选，至少给一个；avatar 为空串=清除头像）
→ `200 {"user":user 对象}`；错误：400 无字段/昵称为空。
> 头像接口存“相对路径/文件名”，图片文件由前端保存到其数据目录 `avatars/` 后再把路径传来。

### 12. 未结算订单查询（前端“去结算/拦截”用）
`GET /api/users/{userId}/active-order`
→ `200 {"order":order 对象 | null}`（null 表示当前没有待支付/充电中的订单）

---

## 二、管理端（先登录拿 token）

管理端请求都要带请求头：`Authorization: Bearer <token>`

### 13. 管理员登录
`POST /api/admin/login`　body:`{"account":"admin","password":"123456"}`
→ `200 {"token":"...","account":"admin"}`；错误：401 账号或密码错误。

### 14. 退出登录
`POST /api/admin/logout`

### 15. 用户列表（模糊搜索）
`GET /api/admin/users?keyword=139`
→ `200 {"users":[user 对象...]}`

### 16. 冻结 / 解冻用户
`PUT /api/admin/users/{userId}/status`　body:`{"frozen":true}`
→ `200 {"userId":6,"frozen":true}`

### 17. 营收 + 电桩状态总览
`GET /api/admin/stats`
→ `200 {"revenue":{"today","month","total"},"charger":{"idle","connected","charging","fault","total"}}`

### 18. 近 N 天营收（折线图）
`GET /api/admin/stats/daily?days=7`
→ `200 {"daily":[{day,amount,orders}]}`（无数据日补 0）

### 19. 按电站营收排行（近 N 天）
`GET /api/admin/stats/by-station?days=30`
→ `200 {"items":[{id,stationId,name,amount,orders}...]}`（金额降序）

### 20. 按电桩营收排行（近 N 天）
`GET /api/admin/stats/by-charger?days=30`
→ `200 {"items":[{id,chargerId,name,桩编号,amount,orders}...]}`（金额降序）

### 21. 新增电站
`POST /api/admin/stations`　body:`{"name","codePrefix","address","longitude","latitude","price"}`
→ `201 {"stationId":6,"message":"新增充电站成功"}`

### 22. 修改电站
`PUT /api/admin/stations/{stationId}`　body 同上
→ `200 {"message":"修改充电站成功"}`

### 23. 删除电站
`DELETE /api/admin/stations/{stationId}`
错误：409 站下仍有电桩(BR-10)。

### 24. 批量建桩（自动编号 缩写-序号）
`POST /api/admin/stations/{stationId}/chargers/batch`　body:`{"count":5,"type":0,"power":7}`
→ `201 {"created":5,"message":"批量建桩成功"}`

### 25. 删除电桩
`DELETE /api/admin/chargers/{chargerId}`
错误：409 有进行中订单/已有历史订单。

### 26. 电桩故障 / 恢复（远程重启）
`PUT /api/admin/chargers/{chargerId}/action`　body:`{"action":"fault"|"recover"|"restart"}`
→ `200 {"chargerId","status","statusText"}`
注意：只能对“空闲”桩标记故障；恢复只能对“故障”桩。非法转换返回 409。
**每次成功都会自动写入一条运维日志(ops_log)，记录操作账号与动作。**

### 27. 管理端订单列表
`GET /api/admin/orders?userId=1&status=2`（参数都可选）
→ `200 {"orders":[order 对象...]}`

### 28. 运维日志列表
`GET /api/admin/logs?limit=50`（最近 limit 条，倒序）
→ `200 {"logs":[{logId,adminAccount,chargerId,chargerCode,action,detail,createdAt}]}`

### 29. 演示数据生成（开发/演示专用，不属于正式业务）
`POST /api/admin/demo/history` 请求体 `{"days":180, "density":1.0}`（都可选，days 默认 30、范围 1~730；density 0.1~20）
→ `200 {"demoOnly":true,"daysRequested":180,"daysGenerated":150,"ordersAdded":..,"rechargesAdded":..,"opsLogsAdded":..,"predictionsAdded":..}`

> 作用：把 `charging_order` 历史**向前补足**到最近 `days` 天（已有数据不动，幂等可重跑），
> 同一窗口顺手补齐 `recharge_log / ops_log / load_prediction`——这三张表平时只有真实业务
> 运行才会积累，新库默认是空的。它只是“造演示历史”的工具（离线命令等价写法：
> `QtDatabase.exe --gen-history 180`），**不是**真实业务的增删改通道。

---

## 三、一个完整充电流程示例（前端）

```
① POST /api/users/login {"phone":"13900000001"}     → token、userId
② GET  /api/chargers?stationId=1                     → 挑一台 status=0 的空闲桩 chargerId
③ POST /api/charges {"chargerId":..}  (带用户 token)  → orderId（状态：待支付/已连接）
   —— 若已有未结算订单，③ 会返回 409，可先用 ⑧ active-order 查到它并提示“去结算”
④ POST /api/charges/{orderId}/start  (带 token)      → 开始充电（充电中）
⑤ 客户端每秒定时器模拟计电、等几分钟
⑥ POST /api/charges/{orderId}/finish (带 token)      → 结算：返回电量/费用/站名桩号（已完成）
```

## 四、错误码使用建议（前端）

- 400：检查参数；401：跳登录；403：账号被冻结，或无权操作他人数据；
- 409：按 `error` 文案提示（如“您有未完成的充电订单…”“余额不足，请先充值”）；
- 404：资源不存在；其它：提示“服务异常，请稍后再试”。

## 五、已实现/预留小结

已实现：用户端 12 个（登录/电站/附近电站/电桩/下单/开始/结算/取消/订单/充值/资料/未结算查询），
管理端 16 个（登录/退出/用户管理/统计 4 类/电站桩管理/运维日志）。
预留（按需再加）：预约超时自动释放、真实图片上传(目前头像只存相对路径)、
设备心跳/告警、微信支付/发票/收藏等。欠费记录(BR-06)已实现：余额不足时订单记录 debt。

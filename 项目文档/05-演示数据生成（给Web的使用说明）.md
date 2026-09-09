# 演示历史数据生成 —— 使用说明（给 Web / 前端同学）

> 数据库模块的人不再手动造数了：以下两种方式你（Web/前端）自己就能加数据。
> 重要前提：**这个功能只造"过去的历史"，不改任何进行中的数据**。订单/充值/运维/负荷预测
> 这些"业务数据"的正常增删改仍然走 REST 接口，本工具只是给图表演示批量补历史，
> 接口返回都带 `"demoOnly": true`，前端不要把它当正式业务。

## 一、能造什么、不能造什么

| 表 | 效果 | 说明 |
|---|---|---|
| `charging_order` 订单 | ✅ 历史往前补 | 只新增"已完成/已取消"的旧订单，时间分布真实（周末多、早晚高峰） |
| `recharge_log` 充值流水 | ✅ 补齐 | 平时只有真实充值才会积累，新库默认是空的 |
| `ops_log` 运维日志 | ✅ 补齐 | 同上（故障/恢复/重启/巡检） |
| `load_prediction` 负荷预测 | ✅ 补齐 | 每站每天 6 个时段，给预测曲线用 |
| user / station / charger 主数据 | ❌ 不动 | 主数据是"底数"，不该用这个工具改 |
| 进行中订单 / 电桩状态 / 余额欠费 | ❌ 不动 | 这些属于业务，仍走接口 |
| 遥测/心跳（实时数据） | ❌ 不造 | 那是模拟器运行时产生的，造假历史没意义 |

**幂等**：命令/接口可重复运行。只补"还没覆盖的更早日期"，已覆盖就不再加。
比如先 `--gen-history 180`，再跑一次 180，会提示"已覆盖，无需生成"。

## 二、方式 A：命令行（推荐你本地跑，或让数据库同学跑一次）

在平台 exe 所在目录执行（先确认平台已用最新代码构建）：

```bash
# 把历史补到最近 365 天(默认种子已含近 30 天, 这条会在 30 天基础上再往老补 335 天)
QtDatabase.exe --gen-history 365

# 想"每天的量"更多, 加密度(0.1~20, 默认 1.0):
QtDatabase.exe --gen-history 365 --density 1.5

# 数据库不在默认位置时, 显式指定:
QtDatabase.exe --db D:\demo\charge_platform.db --gen-history 180
```

- `days` 范围 1~730；`density` 越大每天单量越多（默认 1.0 ≈ 全站每天约 250 单）。
- 跑完会打印类似：`历史数据补生成完成: 新增 275 天(订单 +xx, 充值流水 +xx, 运维日志 +xx, 负荷预测 +xx)`。
- 量级参考：90 天库约 4.7MB；365 天大约十几 MB，SQLite 完全没问题。
- ⚠️ **拷 exe 到别的机器/目录跑时，一定要用 `--db` 指到真实库文件**，否则程序会到
  编译时写死的路径（`C:\AIPlace\QtDatabase\charge_platform.db`）找库，那台机器上没有
  就会新建一个空库，等于白跑。

## 三、方式 B：REST 接口（你前端自己调，最省事）

不用碰 exe，Web 端登录管理账号后直接调：

```http
POST /api/admin/demo/history
Authorization: Bearer <管理员token>
Content-Type: application/json

{ "days": 365, "density": 1.0 }
```

完整流程（curl 示例，服务器默认 8080）：

```bash
# 1. 管理员登录拿 token(默认账号 admin / 123456)
curl -X POST http://127.0.0.1:8080/api/admin/login \
  -H "Content-Type: application/json" \
  -d '{"account":"admin","password":"123456"}'
# → {"token":"abcdef..."}

# 2. 生成历史(带上 token)
curl -X POST http://127.0.0.1:8080/api/admin/demo/history \
  -H "Authorization: Bearer abcdef..." \
  -H "Content-Type: application/json" \
  -d '{"days":365,"density":1.0}'
```

成功响应示例：

```json
{
  "demoOnly": true,
  "message": "演示历史数据已生成",
  "daysRequested": 365,
  "daysGenerated": 275,
  "ordersAdded": 64500,
  "rechargesAdded": 890,
  "opsLogsAdded": 700,
  "predictionsAdded": 82500
}
```

如果已经覆盖（比如再跑一次同样的天数）：
`"message": "历史已覆盖, 无需生成(演示数据)"`，`daysGenerated: 0`。

参数校验：`days` 1~730；`density` 0.1~20，超范围返回 400；未登录/无权限返回 401。
如果数据库同学还没启动服务器，你自己也可以把它当前端"造数按钮"接上（开发/演示期用，
正式页面上藏起来即可）。

## 四、怎么确认"补够了"

- 再次调用同一参数，返回 `daysGenerated: 0` 说明已覆盖；
- 或直接看最老一笔订单日期：`SELECT MIN(start_time) FROM charging_order WHERE start_time IS NOT NULL;`
  比 `今天 - 365 天` 更早就说明 365 天已覆盖。

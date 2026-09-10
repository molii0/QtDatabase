# ChargerSimulator —— 充电桩设备仿真（Device Side）

> 一个**独立进程**的充电桩模拟程序：把原来"软件里假装的充电桩"
> 提升为具有独立设备边界的 **Charger Device Simulator**（见 `需求.md`）。
>
> **v2 架构约定：模拟层只与数据库层对接**——遥测/心跳/设备状态/故障日志
> 全部写入平台数据库；接口层、界面层等其他层不与设备直连，一律经由数据库层
> 读取这些数据。将来换成真实 Embedded Linux 终端时，只需保持这套
> 表结构与命令词汇不变，上层无需改动。

## 一、v2 相比 v1 做了什么

v1 只把遥测/心跳帧打印到控制台；v2 补齐了 `README` 下一步的第 1、2 项，
**设备数据真正入库、双向通道打通**：

| # | 功能 | 说明 |
|---|------|------|
| 1 | 设备状态机 | `IDLE→RESERVED→CHARGING→FINISHED→IDLE`，任意→`FAULT`→`IDLE`，任意→`OFFLINE`→`IDLE`（v1 已有） |
| 2 | 设备自运行 | 1 秒定时器驱动，SoC/电量/温度自己涨，**功率取数据库里该桩的功率**（快充/慢充自然区分） |
| 3 | 遥测入库 | 每秒一帧写入 `charger_telemetry` 表（含控制台 `[TELEM]` 回显），每桩自动裁剪只留最近 1000 帧 |
| 4 | 心跳入库 | 每 5 秒 upsert `charger_heartbeat`（每桩一行）；**平台按 `now - last_seen` 超过 15 秒判定设备离线** |
| 5 | 状态投影 | 设备 6 态映射到 `charger.status` 4 态（见下）；**有进行中订单的桩不覆盖**（归订单流程管） |
| 6 | 故障运维日志 | 设备故障上报/恢复写入 `ops_log`（admin_account = `device`），管理端运维日志页可见 |
| 7 | **命令通道** | 平台调 `DBManager::pushDeviceCommand` 写入 `device_command` 表，设备每秒轮询执行并回填结果；控制台输入仍是"本地物理操作" |
| 8 | 掉线模拟 | `offline` 后遥测/心跳停止（平台靠心跳超时发现）；下发命令保持待执行，重新上线后补执行 |
| 9 | 多设备模拟 | `--devices N` 按序绑定数据库电桩（默认 3 台），`--station` 可只绑某电站；设备 id = `charger_id` |
| 10 | 自动演示 | `--auto`：随机插枪充电、随机故障（自动恢复）、随机掉线（自动上线），免手打演示 |

## 二、架构与数据流

```text
┌────────────────────────────────┐
│  ChargerSimulator(设备层进程)   │
│  charger 状态机 × N 台          │
└──────────────┬─────────────────┘
               │ 只对接数据库层(直接编译 DBManager 源码)
               ▼
┌────────────────────────────────┐     其他层(REST API / Qt 界面)
│  数据库层 DBManager + SQLite    │──── 由数据库层对接, 不与设备直连:
│  charger_telemetry  遥测        │     读遥测/心跳做展示与离线判定,
│  charger_heartbeat  心跳        │     调 pushDeviceCommand 下发命令
│  charger.status     状态投影     │
│  ops_log            故障日志     │
│  device_command     命令通道     │
└────────────────────────────────┘
```

**设备 6 态 → charger.status 4 态映射**（`DeviceDb::projectStatus`）：

| 设备状态 | charger.status | 说明 |
|---|---|---|
| idle | 0 空闲 | |
| reserved | 1 已连接 | 枪已插 |
| charging | 2 充电中 | |
| finished | 1 已连接 | 充完等拔枪 |
| fault | 3 故障 | |
| offline | 不投影 | 设备失联、停止上报，由心跳超时判定 |

> 订单保护：`syncChargerDeviceStatus` 对有进行中订单（待支付/充电中）的桩
> 跳过写入——这些桩的状态由订单流程（下单/开始/结算）负责，两条链路互不覆盖。

## 三、目录结构

```text
ChargerDeviceSimulator/
├── 需求.md
├── README.md
├── ChargerSimulator.pro      # qmake 工程(引入上层仓库的 DBManager 数据库层)
└── src/
    ├── main.cpp              # 入口: 参数解析 / 默认数据库路径解析
    ├── simulator.h/.cpp      # 多设备管理器: 定时驱动、入库、命令轮询、--auto
    ├── charger.h/.cpp        # 单个设备: 状态机 + 自运行物理模拟
    └── devicedb.h/.cpp       # 设备<->数据库桥: 遥测/心跳/状态投影/命令领取
```

## 四、构建与运行

环境：Qt 6.11.2 (MinGW 64-bit) + Qt Creator，与平台工程一致（数据库层源码
直接从上层仓库编译进本程序，无需先构建平台）。

1. 用 Qt Creator 打开 `ChargerSimulator.pro` 构建运行；或命令行 `qmake && mingw32-make`。
2. 命令行参数：
   - `--db FILE`：平台数据库文件；不传则按默认规则挑：**当前目录已有库 > exe 目录
     已有库 > 工程根目录** `charge_platform.db`（`.pro` 里 `DEFAULT_DB_DIR` 固定，
     不存在会自动建库并写演示数据：5 站 × 6 桩 ≈ 30 台桩）。多进程共用时建议显式
     `--db` 指向同一个文件（WAL 支持两进程并发）
   - `--print-db`：只打印将使用的数据库文件路径后退出（排查“连的哪个库”）
   - `--station ID`：只模拟该电站下的电桩
   - `--devices N`：最多绑定电桩数（默认 3，上限 30）
   - `--duration SEC`：运行 N 秒后自动退出（自检/演示用）
   - `--auto`：随机场景自动演示
   - `--help`：帮助

```bash
# 例: 模拟 1 号电站的 4 台桩, 自动演示, 跑 60 秒(不传 --db 时按上面的默认规则挑库)
ChargerSimulator.exe --station 1 --devices 4 --auto --duration 60

# 也可显式指到平台库(位置任意, 例如放在别的目录的副本)
ChargerSimulator.exe --db D:\demo\charge_platform.db --devices 4 --auto
```

## 五、交互命令（本地"物理操作"；id 为数据库电桩编号 charger_id）

| 命令 | 说明 | 状态变化 |
|------|------|----------|
| `list` | 查看所有设备状态表 | - |
| `db` | 查看数据库路径与已发送帧数 | - |
| `plug <id>` | 插枪 | idle → reserved |
| `start <id>` | 开始充电 | reserved → charging |
| `stop <id>` | 手动结束充电 | charging → finished |
| `unplug <id>` | 拔枪 | finished → idle |
| `fault <id> [code]` | 模拟故障（默认码 1） | 任意 → fault |
| `recover <id>` | 故障恢复 | fault → idle |
| `offline <id>` | 模拟掉线/断电（遥测、心跳停止） | 任意 → offline |
| `online <id>` | 恢复在线 | offline → idle |
| `restart <id>` | 重启（上电自检约 2 秒） | 任意 → offline → idle |
| `exit` | 退出 | - |

演示一段"完整充电"：

```text
plug 2        # [ACK] ok,  [EVENT] idle -> reserved
start 2       # [ACK] ok,  [EVENT] reserved -> charging
              # 之后每秒 [TELEM] soc 上涨(按桩功率), temp 升高, 数据入库
              # 充满后自动 [EVENT] charging -> finished, 3 秒后 auto unplug
```

## 六、Server → Device 命令通道（平台怎么下发命令）

平台侧（REST 接口/管理端 Qt）对某台桩下发命令只需两步数据库层调用：

```cpp
qint64 cmdId;
DBManager::instance().pushDeviceCommand(chargerId, "restart", {}, &cmdId);
// 设备 1 秒内轮询到并执行; 执行结果回填 device_command:
//   status: 1 已执行 / 2 执行失败;  result: "ok" 或失败原因;  done_at: 完成时间
DBManager::instance().listDeviceCommands(20, &list);   // 查最近命令与结果
```

支持命令：`plug / start / stop / unplug / fault(参数=故障码) / recover /
offline / online / restart`。设备离线期间命令保持待执行，上线后补执行。

## 七、输出帧格式（控制台回显，与入库数据同源）

```jsonc
// [TELEM] 每秒, 在网设备各一帧 -> charger_telemetry 表
{"device_id":2,"ts":"2026-09-07 20:00:00","status":"charging","power":7.0,
 "soc":35.2,"energy":0.12,"temperature":25.4}
// [HEART] 每 5 秒, 在网设备各一帧 -> charger_heartbeat 表(upsert)
{"device_id":2,"ts":"2026-09-07 20:00:00","status":"charging","uptime_s":125}
// [EVENT] 状态变化 -> 同时把状态投影进 charger.status
[EVENT] device 2: reserved -> charging (session started)
// [ACK]  本地命令应答
[ACK] command=start device=2 ok
// [CMD ] 平台经数据库下发的命令(设备轮询执行后回填 device_command)
[CMD ] #1 server -> device 1: restart
[ACK] (from server) command=restart device=1 ok
// [AUTO] --auto 模式的随机场景动作
[AUTO] command=plug device=3 ok
```

## 八、说明与下一步

- 充电模型刻意保持简单（固定电池容量 60kWh、演示加速 60 倍、无随机数），
  **额定功率取数据库中该桩的 power**（种子数据：桩号奇数快充 60/120kW、偶数慢充 7kW），
  结果可复现、快慢充观感自然区分。
- 设备掉线是"真掉线"：不写任何数据，平台靠 `charger_heartbeat.last_seen`
  超时（建议 15 秒）判定离线——即需求里的 device health monitoring。
- **下一步（可选）**：
  1. 平台侧把 `charger_heartbeat`/`charger_telemetry` 展示到管理端界面（在线状态、遥测曲线）；
  2. 管理端"远程重启/标记故障"按钮改走 `pushDeviceCommand`，由设备真实执行；
  3. 平台按心跳超时把离线设备在界面上标红/告警（离线判定逻辑建议放服务层定时器）；
  4. 网络断开自动重连策略、设备管理界面（当前用控制台 + `--auto`）。

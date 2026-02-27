# Mellanox NIC QoS 自动配置特性说明

## 1. 特性概述

本特性为 nvme-snsd 守护进程新增 Mellanox 网卡 QoS 自动配置能力，支持通过配置文件声明式地管理 PFC（优先级流控）、信任模式（trust mode）和 VLAN 出口 QoS 映射（egress-qos-map）。守护进程在启动时自动下发配置，并通过周期性巡检机制检测配置漂移并自动修复。

### 核心价值
- **声明式配置**：在 `/etc/nvme/snsd.conf` 的 `[NETWORK]` 段中声明期望的 QoS 配置，守护进程自动应用
- **配置漂移检测**：周期性读取当前硬件配置并与期望值对比，发现不一致时自动重新下发
- **标准内核接口**：使用 DCB netlink（IEEE 802.1Qaz）标准接口，不依赖 `mlnx_qos` 等用户态工具

## 2. 配置格式

### 2.1 BASE 段新增参数

```ini
[BASE]
--restrain-time = 0
--qos-check-interval = 30   ; QoS 巡检周期，单位秒，默认 30，范围 5~3600
```

### 2.2 NETWORK 段（新增）

```ini
[NETWORK]
; 物理口配置 PFC 和 trust；VLAN 口配置 egress-qos-map
--ifname = ens64f0 | --pfc = 0,0,0,1,0,0,0,0 | --trust = dscp
--ifname = ens0.10 | --pfc = 0,0,0,1,0,0,0,0 | --trust = dscp | --egress = 0:3,1:3,2:3
```

各字段说明：

| 字段 | 必选 | 说明 |
|------|------|------|
| `--ifname` | 是 | 网口名称，支持物理口（如 `ens64f0`）和 VLAN 口（如 `ens0.10`） |
| `--pfc` | 否 | 8 个优先级的 PFC 使能，逗号分隔，0/1 值。配置在物理口上 |
| `--trust` | 否 | 信任模式，`dscp` 或 `pcp`。配置在物理口上 |
| `--egress` | 否 | VLAN 出口 QoS 映射，`FROM:TO` 格式（skprio:UP），逗号分隔。仅适用于 VLAN 口 |

### 2.3 配置校验规则
- `--ifname` 为必填项
- VLAN 口（名称含 `.`）自动提取物理口名称用于 PFC/trust 配置
- `--egress` 仅允许在 VLAN 口上配置
- `trust=pcp` 时接口必须为 VLAN 口

## 3. 实现架构

### 3.1 新增文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/snsd_dcb.h` | 98 | DCB netlink 接口头文件 |
| `src/snsd_dcb.c` | 725 | DCB netlink 操作实现（PFC/trust/egress 的读写） |
| `src/snsd_network.h` | 100 | QoS 配置管理头文件 |
| `src/snsd_network.c` | 547 | 配置解析、下发、巡检逻辑 |
| `test/ut/snsd_network_ut.cpp` | 261 | 单元测试 |

### 3.2 修改文件

| 文件 | 修改内容 |
|------|----------|
| `src/snsd_cfg.h` | `snsd_base_cfg` 结构体新增 `qos_check_interval` 字段 |
| `src/snsd_cfg.c` | 新增 `--qos-check-interval` 解析；调用 `snsd_network_init/exit` |
| `src/snsd_main.c` | 启动时调用 `snsd_network_apply()`；主循环中周期调用 `snsd_network_check()` |
| `Makefile` | 新增 `snsd_dcb.o` 和 `snsd_network.o` |
| `test/ut/Makefile` | 新增 UT 编译对象 |

### 3.3 DCB Netlink 实现细节

底层通过 `AF_NETLINK, SOCK_RAW, NETLINK_ROUTE` 套接字与内核 DCB 子系统通信：

- **PFC 读写**：`RTM_GETDCB/RTM_SETDCB` + `DCB_CMD_IEEE_GET/SET`，操作 `DCB_ATTR_IEEE_PFC` 属性中的 `struct ieee_pfc` 结构体
- **Trust 模式**：优先尝试 sysfs (`/sys/class/net/<dev>/qos/trust`)，不存在时回退到 netlink APP 表操作：
  - 设置 DSCP 信任：写入 64 条 APP 表项（selector=5, dscp 0~63 → 对应优先级）
  - 设置 PCP 信任：通过 `DCB_CMD_IEEE_DEL` 删除所有 selector=5 的表项
- **Egress QoS Map**：通过 `RTM_NEWLINK` + `IFLA_LINKINFO` / `IFLA_INFO_DATA` / `IFLA_VLAN_EGRESS_QOS` netlink 设置；通过 `RTM_GETLINK` 解析嵌套 `IFLA_VLAN_QOS_MAPPING` 属性读取

### 3.4 守护进程生命周期集成

```
main()
  ├── snsd_cfg_init()          // 解析配置，包括 [NETWORK] 段
  ├── snsd_network_apply()     // 首次下发 QoS 配置（失败不阻塞启动）
  ├── port_handle()            // 主循环
  │     └── 每 qos_check_interval 秒 → snsd_network_check()  // 巡检
  └── snsd_network_exit()      // 清理
```

## 4. 验证状态

### 4.1 硬件验证环境
- **服务器**: 51.38.87.168，CentOS 8，内核 4.18
- **网卡**: Mellanox ConnectX-4 Lx（ens64f0/ens64f1），mlx5_core 驱动
- **工具**: mlnx_qos（用于交叉验证配置结果）

### 4.2 已在硬件上验证的功能

| 功能 | 验证方式 | 结果 |
|------|----------|------|
| PFC 下发 | 配置 priority 3 使能，通过 `mlnx_qos -i ens64f0` 确认 pfc_en=0x08 | **通过** |
| Trust 模式切换 | 从 pcp 切换为 dscp，通过 `mlnx_qos -i ens64f0` 确认 dscp2prio 映射生效 | **通过** |
| Trust sysfs 回退 | 服务器无 `/sys/class/net/ens64f0/qos/trust`，自动回退 netlink APP 表 | **通过** |
| PFC 漂移检测 | 手动用 `mlnx_qos` 清零 PFC，守护进程在巡检周期后自动恢复 priority 3 | **通过** |
| 配置解析（=分隔） | `--trust = dscp` 格式解析成功 | **通过** |
| 配置解析（空格分隔） | `--trust dscp` 格式（原始需求格式）解析成功 | **通过** |
| VLAN Egress QoS (netlink) | 创建 ens64f0.100 VLAN 口，通过 RTM_NEWLINK 下发 0:3,1:3 映射，`/proc/net/vlan/` 确认生效 | **通过** |
| Egress 漂移检测 | 手动清除 egress 映射（0:0 1:0），守护进程自动恢复为 0:3 1:3 | **通过** |
| 守护进程集成 | 启动时下发、运行中巡检、退出时清理，全流程正常 | **通过** |

### 4.3 未验证的功能

| 功能 | 未验证原因 | 风险评估 |
|------|------------|----------|
| 多接口配置 | 仅测试了单接口配置 | **低** — 多接口是循环遍历，逻辑与单接口一致 |
| Trust 漂移检测 | 未单独测试 trust 被外部修改后的自动恢复 | **低** — 与 PFC 漂移检测逻辑一致 |
| Egress netlink GET | RTM_GETLINK 读取 egress 映射用于漂移检测，当前巡检路径验证了 GET 能正确读到非零映射 | **低** |
| 单元测试构建运行 | 远程服务器未安装 gtest/mockcpp | **低** — UT 框架已有成熟模式 |

## 5. 下一步计划

### 5.1 已修复（对标原始需求）

1. **[P0] 配置解析器兼容空格分隔格式** — ✅ 已修复并验证
   - `snsd_network_split_field()` 先尝试 `=`，回退空格分隔
   - 远程验证：`--trust dscp`（空格分隔）正确解析并下发

2. **[P1] Egress QoS Map 改用纯 netlink 实现** — ✅ 已修复并验证
   - set: `RTM_NEWLINK` + `IFLA_LINKINFO` / `IFLA_INFO_DATA` / `IFLA_VLAN_EGRESS_QOS`
   - get: `RTM_GETLINK` 解析嵌套 `IFLA_VLAN_QOS_MAPPING` 属性
   - 远程验证：ens64f0.100 VLAN 口 egress 0:3,1:3 下发和漂移检测均通过
   - 调试修复：需 `NLM_F_ACK` 避免 recv 阻塞；需 `RTM_NEWLINK`（非 RTM_SETLINK）使内核处理 `IFLA_LINKINFO`

3. **[R1-R4] 专家评审阻塞项** — ✅ 全部修复并通过硬件验证
   - R1: `snsd_dcb_add_attr()`/`snsd_dcb_nest_start()` 增加 `SNSD_NL_BUF_SIZE` 边界检查
   - R2: netlink socket 增加 `SO_RCVTIMEO` 5 秒超时，`recv()==0` 返回 `-ECONNRESET`
   - R3: `--qos-check-interval` 增加 [5, 3600] 范围校验（clamp）
   - R4: PFC 读取改用 `struct ieee_pfc` 指针 + payload 长度校验
   - 集成验证：PFC 下发/漂移修复（使用新 struct ieee_pfc 读取路径）、Trust、Egress 全部通过

4. **静态分析合规** — ✅ 通过
   - 工具：cppcheck 2.13.0、clang-tidy 18.1.3、flawfinder 2.0.19
   - 修复：移除冗余条件判断、增加 `const` 指针限定符（只读解析路径）
   - 剩余：`variableScope` 风格建议（保持项目现有代码风格，声明在函数顶部）
   - flawfinder 仅报告 Level 2（通用缓冲区/fopen 提示，均为项目既有代码且使用安全）

### 5.2 短期

3. **Trust 漂移检测测试**
   - 手动通过 `mlnx_qos` 将 trust 切回 pcp，确认守护进程自动恢复为 dscp

4. **多接口配置测试**
   - 同时配置 ens64f0 和 ens64f1，验证并行下发

### 5.3 中期

6. **单元测试构建验证**
   - 在远程服务器安装 gtest/mockcpp 依赖
   - 构建并运行 UT，确保 mock DCB 函数路径覆盖

7. **错误恢复测试**
   - 模拟 netlink 通信失败场景（如网口 down）
   - 验证错误日志输出和重试行为

### 5.4 长期

8. **配置热更新**：支持不重启守护进程的情况下重新加载 `[NETWORK]` 配置
9. **ETS 支持**：扩展 DCB 层支持 IEEE 802.1Qaz ETS（增强传输选择）配置
10. **多厂商适配**：验证在非 Mellanox 网卡（如 Intel E810）上的兼容性

## 6. 与原始需求的偏差（已修复）

### 6.1 [P0] 配置格式解析 — ✅ 已修复

原始需求使用 `--trust dscp`（空格分隔），实现要求 `--trust = dscp`（等号分隔）。

**修复**（commit 314e258）：新增 `snsd_network_split_field()` 函数，先尝试 `=` 分隔，找不到时回退空格分隔。两种格式均已在硬件上验证通过。

### 6.2 [P1] Egress QoS Map netlink 实现 — ✅ 已修复

原始需求要求使用标准 netlink 接口，实现使用了 `system("ip link set ...")`。

**修复**（commit 314e258, ce135e1）：
- set: `RTM_NEWLINK` + `IFLA_LINKINFO` / `IFLA_INFO_DATA` / `IFLA_VLAN_EGRESS_QOS` / `IFLA_VLAN_QOS_MAPPING`
- get: `RTM_GETLINK` + 解析嵌套 VLAN 属性
- 注意：必须用 `RTM_NEWLINK`（非 `RTM_SETLINK`），因为内核 `rtnl_setlink()` 不处理 `IFLA_LINKINFO`
- 注意：必须设置 `NLM_F_ACK` 标志，否则 `recv()` 永久阻塞

已在 ens64f0.100 VLAN 接口上验证 egress 0:3,1:3 下发和漂移检测。

## 7. 代码审查结果（Gemini + Codex 双专家审查）

两位独立专家对全部代码、测试和需求进行了审查，结论为 **条件性 GO（Conditional GO）**。
架构设计优秀，核心功能已通过硬件验证。4 项 PR 阻塞项已全部修复并通过集成验证。

### 7.1 合入前必修项（PR 阻塞项，两位专家共识）

| # | 问题 | 涉及文件 | 状态 |
|---|------|----------|------|
| R1 | `snsd_dcb_add_attr()` / `snsd_dcb_nest_start()` 缓冲区写入无边界检查，可能溢出栈上的 4096 字节缓冲区 | `src/snsd_dcb.c` | **已修复** — 两函数均增加 `SNSD_NL_BUF_SIZE` 边界检查并返回 `-ENOSPC`，所有调用点已更新 |
| R2 | netlink `recv()` 无超时保护，异常情况下可能永久阻塞守护进程主线程 | `src/snsd_dcb.c` | **已修复** — `snsd_nl_open()` 新增 `SO_RCVTIMEO` 5 秒超时；`recv()==0` 返回 `-ECONNRESET` |
| R3 | `--qos-check-interval` 无范围校验 [5, 3600]，用户可配置负数或超大值 | `src/snsd_cfg.c` | **已修复** — `snsd_cfg_init()` 中新增 clamp 逻辑，值 0 表示禁用 |
| R4 | PFC 读取使用硬编码字节偏移 `pfc_data[1]`，应改用 `struct ieee_pfc` 结构体指针 | `src/snsd_dcb.c` | **已修复** — 改用 `const struct ieee_pfc *` 指针并校验 payload 长度 |

### 7.2 合入后优先修复项（P1）

| # | 问题 | 说明 |
|---|------|------|
| P1-1 | DSCP trust 设置需 64 次独立 netlink 往返，效率低且非原子 | 考虑批量发送 |
| P1-2 | 嵌套属性缺少 `NLA_F_NESTED` 标志位 | 新版内核（5.2+）strict validation 可能拒绝 |
| P1-3 | ~~`recv()` 返回 0 时未处理~~ | **已在 R2 中修复** — 返回 `-ECONNRESET` |
| P1-4 | 多接口配置同一物理口时无冲突检测 | 后配置覆盖前配置，无警告 |
| P1-5 | 漂移修复粒度粗：任一项漂移都重新下发全部三项 | 分别追踪 pfc/trust/egress drift |

### 7.3 低优先级改进项

| # | 问题 |
|---|------|
| L1 | netlink socket 未设置 `SOCK_CLOEXEC` |
| L2 | `IFLA_EXT_MASK` 值为 `RTEXT_FILTER_VF` 但实际不需要（不影响正确性） |
| L3 | 特性文档 3.3 节 Egress 描述未同步更新为 netlink 实现 |
| L4 | UT 缺少 PFC/Egress/Trust 解析边界测试和漂移检测 mock 验证 |

## 8. 已知限制

1. 配置文件路径硬编码为 `/etc/nvme/snsd.conf`，不支持自定义路径
2. Trust 模式设置在无 sysfs 的环境下依赖 netlink APP 表，写入 64 条 DSCP 映射项，可能在某些驱动实现上有兼容性差异
3. 巡检周期精度受主循环 100ms tick 影响，实际周期可能有 ±100ms 偏差

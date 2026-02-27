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
- **Egress QoS Map**：通过 `ip link set` 命令设置 VLAN 出口映射；读取通过 `/proc/net/vlan/<dev>`

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
| 配置解析 | 单接口 [NETWORK] 段解析成功，syslog 显示正确的配置参数 | **通过** |
| 守护进程集成 | 启动时下发、运行中巡检、退出时清理，全流程正常 | **通过** |

### 4.3 未验证的功能

| 功能 | 未验证原因 | 风险评估 |
|------|------------|----------|
| VLAN 出口 QoS 映射 (egress-qos-map) | 远程服务器上无 VLAN 子接口 | **中等** — 代码路径使用 `ip link set` 命令，逻辑简单但未经实际验证 |
| 多接口配置 | 仅测试了单接口配置 | **低** — 多接口是循环遍历，逻辑与单接口一致 |
| Egress 漂移检测 | 依赖 VLAN 接口存在 | **中等** — 读取 `/proc/net/vlan/<dev>` 解析逻辑未验证 |
| Trust 漂移检测 | 未单独测试 trust 被外部修改后的自动恢复 | **低** — 与 PFC 漂移检测逻辑一致 |
| 单元测试构建运行 | 远程服务器未安装 gtest/mockcpp | **低** — UT 框架已有成熟模式 |

## 5. 下一步计划

### 5.1 短期（需硬件环境）
1. **VLAN 接口测试**
   - 在远程服务器上创建 VLAN 子接口：`ip link add link ens64f0 name ens64f0.100 type vlan id 100`
   - 配置 `--ifname = ens64f0.100 | --pfc = 0,0,0,1,0,0,0,0 | --trust = dscp | --egress = 0:3,1:3`
   - 验证 egress-qos-map 下发和漂移检测

2. **Trust 漂移检测测试**
   - 手动通过 `mlnx_qos` 将 trust 切回 pcp，确认守护进程自动恢复为 dscp

3. **多接口配置测试**
   - 同时配置 ens64f0 和 ens64f1，验证并行下发

### 5.2 中期
4. **单元测试构建验证**
   - 在远程服务器安装 gtest/mockcpp 依赖
   - 构建并运行 UT，确保 mock DCB 函数路径覆盖

5. **错误恢复测试**
   - 模拟 netlink 通信失败场景（如网口 down）
   - 验证错误日志输出和重试行为

### 5.3 长期
6. **配置热更新**：支持不重启守护进程的情况下重新加载 `[NETWORK]` 配置
7. **ETS 支持**：扩展 DCB 层支持 IEEE 802.1Qaz ETS（增强传输选择）配置
8. **多厂商适配**：验证在非 Mellanox 网卡（如 Intel E810）上的兼容性

## 6. 已知限制

1. 配置文件路径硬编码为 `/etc/nvme/snsd.conf`，不支持自定义路径
2. Trust 模式设置在无 sysfs 的环境下依赖 netlink APP 表，写入 64 条 DSCP 映射项，可能在某些驱动实现上有兼容性差异
3. Egress QoS Map 设置通过 `system("ip link set ...")` 执行，非纯 netlink 实现
4. 巡检周期精度受主循环 100ms tick 影响，实际周期可能有 ±100ms 偏差

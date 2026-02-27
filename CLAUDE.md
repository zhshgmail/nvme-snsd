# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

nvme-snsd (NVMe Storage Network Service Discovery) is a Linux daemon (C, gnu99) that automates NVMe over Fabric target discovery, connection, and path failover. It monitors network link changes and automatically creates/switches NVMe-oF connections on host machines. Supports RoCE, TCP, and iSCSI transport protocols. Deployed as a systemd service via RPM. Licensed BSD 3-Clause (Huawei).

## Build Commands

```bash
# Build for x86_64
./build/build_x86_64.sh

# Build for ARM (aarch64)
./build/build_arm.sh

# Build binary only (no RPM packaging)
make

# Clean
make clean
```

## Unit Tests

Tests use Google Test (gtest) and mockcpp. Run from `test/ut/`:

```bash
cd test/ut
./snsd_ut.sh          # Build, run all tests, generate coverage report (requires lcov)

# Or manually:
make                  # Build test binary
./snsd_ut             # Run tests
make clean            # Clean test artifacts
```

## Compiler Flags

The project enforces `-Wall -Werror` — all warnings are errors. Security hardening flags are enabled: stack protector, PIE, RELRO, no-execstack.

## Architecture

The daemon's main loop (`snsd_main.c:port_handle`) polls two network types in a loop:

- **Switch network (SW)** — `snsd_switch.c` — Monitors switched fabric networks via LLDP-like discovery. Detects storage targets appearing on switch ports.
- **Direct connect (DC)** — `snsd_direct.c` — Monitors directly connected networks. Uses configured host-to-target IP mappings.

Both paths feed into the connection management layer:

- **snsd_connect.c** — Connection dispatch layer, routes to protocol-specific handlers
- **snsd_conn_nvme.c** — NVMe-oF connection management (discover/connect/disconnect via nvme-cli)
- **snsd_conn_peon.c** — iSCSI (peon) connection management

Supporting modules:

- **snsd_cfg.c** — Parses `/etc/nvme/snsd.conf` (BASE/SW/DC sections)
- **snsd_server.c** — Netlink socket listener for network link up/down events; manages per-interface worker threads
- **snsd_mgt.c** — Network interface management (bonding, VLAN detection, IP matching)
- **snsd_reg.c** — LLDP frame registration/deregistration with the kernel
- **snsd_nvme.c** — NVMe subsystem/controller sysfs queries
- **snsd_log.c** — Syslog-based logging

## Configuration

Config file: `/etc/nvme/snsd.conf` (sample at `test/config/snsd.conf`)

Three sections: `[BASE]` (global defaults), `[SW]` (switch networks), `[DC]` (direct connect networks). Fields within each line are `|`-separated. Key required fields: `--host-traddr`, `--protocol`, and `--traddr` (DC only).

## Packaging

`build/build.sh` compiles, packages into a zip, then builds an RPM via `script/rpm_build.sh`. The systemd unit file is at `script/nvme-snsd.service`. Version is defined in `SNSD-VERSION-GEN`.

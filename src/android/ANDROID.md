# OpenVPN 2 Android（ics-openvpn）remote-srv 支持

为 [ics-openvpn](https://github.com/schwabe/ics-openvpn)（OpenVPN 2.x C 引擎）增加 DNS SRV 自动发现：配置里写 `remote-srv _service._proto.name`，客户端在建连前查询 SRV，按 RFC 2782 选出一条记录，再以普通 `remote <target> <port> <proto>` 建连。

- 已验证版本：ics-openvpn `v0.7.64`（ovpn2 风味，底层 OpenVPN `2.7_rc6` fork）
- 产物：`bin/ics-openvpn-0.7.64-remote-srv-arm64-v8a-debug.apk`
- 桌面端（OpenVPN GUI / Linux）不走补丁，用 C shim，见 [../../README.md](../../README.md)

## 补丁文件

| 补丁 | 作用 |
|------|------|
| [remote-srv-android.patch](remote-srv-android.patch) | 识别并解析 `remote-srv`：C 引擎层 + Java 应用层 |
| [profile-list-sync.patch](profile-list-sync.patch) | 修复配置列表在界面进程与 `:openvpn` 进程间不同步的问题 |

## 应用补丁

补丁路径相对仓库根目录，已包含 `main/src/main/cpp/openvpn/...`，无需进入子模块单独打补丁。

```bash
git clone --recursive https://github.com/schwabe/ics-openvpn.git
cd ics-openvpn
git apply /path/to/remote-srv-android.patch
git apply /path/to/profile-list-sync.patch
```

## 架构：为什么分两层

OpenVPN 2 的配置在客户端侧生成，`remote-srv` 有两处可介入：

1. **C 引擎层**（`srv.c`）：在 `link_socket_init_phase1` 建连前解析 SRV，直接替换 `remote_host` / `remote_port` / `proto`。桌面端与自编 openvpn 走这条路径。
2. **Java 应用层**（`SrvResolver`）：Android 上由 ics-openvpn 生成配置文件，Java 层在生成前把 `remote-srv` 解析成普通 `remote`。

必须加 Java 层的原因：Android 13+ 的 Bionic `res_query()` 依赖已废弃的 `net.dns1` 系统属性，现代设备上该属性为空，SRV 查询必然失败（实测日志：`SRVCHECK: is_srv=1` 后紧接 `remote-srv: SRV query failed`）。

Java 层实现要点：

- 取 `ConnectivityManager.getActiveNetwork()` 对应 `LinkProperties.getDnsServers()`；
- 对每个 DNS 服务器构造 SRV 查询（QTYPE=33）发 UDP 53，3 秒超时，逐个回退；
- 解析应答（处理压缩名），在最低 priority 组内按 weight 加权随机选一条（RFC 2782）；
- 返回 `{target, port, "tcp"|"udp"}`，协议取自查询名里的 `_tcp` / `_udp`。

因此 Android 上引擎侧看到的是 `is_srv=0` 的普通 `remote`，属预期行为，功能等价。

## 补丁改动点

`remote-srv-android.patch`：

| 文件 | 改动 |
|------|------|
| `cpp/openvpn/src/openvpn/srv.c` / `srv.h` | 新增：跨平台 SRV 查询（POSIX `res_query` / Windows `DnsQuery_W`）+ RFC 2782 选路 |
| `cpp/openvpn/src/openvpn/options.h` | `connection_entry.remote_is_srv`、`options.is_srv` |
| `cpp/openvpn/src/openvpn/options.c` | 解析并校验 `remote-srv` 指令 |
| `cpp/openvpn/src/openvpn/socket.c` | 建连前（`link_socket_init_phase1`）解析 SRV |
| `cpp/CMakeLists.txt` | 源文件列表加入 `src/openvpn/srv.c` |
| `java/.../core/Connection.java` | `mRemoteSrv` 标记；生成连接块时输出 `remote-srv` |
| `java/.../core/ConfigParser.java` | 解析 `remote-srv` 并生成独立连接项 |
| `java/.../VpnProfile.java` | 生成配置前调用 `SrvResolver` 解析并改写为普通 `remote` |
| `java/.../core/SrvResolver.java` | 新增：手写 UDP DNS SRV 查询（绕过 Bionic `res_query`） |

`profile-list-sync.patch`：

| 文件 | 改动 |
|------|------|
| `java/.../core/ProfileManager.java` | 配置列表改为扫描 `files/` 下实际存在的 `.cp/.cpold/.vp` 文件，不再只依赖 SharedPreferences 的 `vpnlist` |

## 编译

已验证环境：JDK 21、Android SDK（compileSdk 36）、NDK 29.0.14206865、CMake 3.31.6、Gradle 9.1.0。

```bash
# 在 ics-openvpn 根目录执行
./gradlew :main:assembleUiOvpn2Debug

# 产物
#   main/build/outputs/apk/uiOvpn2/debug/main-ui-ovpn2-arm64-v8a-debug.apk
```

构建配置建议：在 `main/build.gradle.kts` 的 `splits` 中只保留目标 ABI（如 `include("arm64-v8a")`、`isUniversalApk = false`），四 ABI 同时打包会很慢。

## 使用

配置示例（协议与端口来自 SRV 记录，无需写 `remote`）：

```text
client
dev tun
remote-srv _openvpn._tcp.vpn.example.com
proto tcp
ca ca.crt
auth-user-pass
```

导入 APK 后照常连接，账号密码按提示输入。成功时日志中可见：

```text
SRV _openvpn._tcp.vpn.example.com -> 203.0.113.10:1194 tcp   # 调试构建
Attempting to establish TCP connection with [AF_INET]203.0.113.10:1194
STATE: CONNECTED,SUCCESS,10.8.0.6,203.0.113.10,1194
```

> 正式包已移除 `OVPNCFG` / `SRVCHECK` 调试日志。

## 实测记录

设备：Android / arm64-v8a。

- 安装、启动正常，`versionName=0.7.64-o2`。
- `remote-srv _openvpn._tcp.vpn.example.com` → 解析出目标地址与端口，与 PC 端 `Resolve-DnsName` 结果一致。
- TCP 连接 `CONNECTED,SUCCESS`，隧道 `tun0` 建立，`ping` 0% 丢包。
- 普通 `remote` 配置同样连接成功，原有路径未受影响。
- 补丁可复现性：两个补丁应用到原始 `v0.7.64` 源码后，与当前构建源码逐文件一致。

## 已知限制与注意事项

- **C 层 `res_query` 在 Android 上不可用**，仅 Java 层有效；C 层保留是为了桌面端与自编 openvpn。
- **官方 OpenVPN Connect（商店版）不适用**：其底层是 OpenVPN 3（C++ 库），见 [../../OPENVPN3.md](../../OPENVPN3.md)。
- **不要在同一台设备上交替安装不同的 0.7.64 构建**（例如不同 flavor 的包）。同一 package 下 `MODE_MULTI_PROCESS` 的 SharedPreferences 各进程缓存不一致，曾导致索引被写空、引擎报 `Fetched VPN profile ((null))`；`profile-list-sync.patch` 已让列表以磁盘文件为准，但仍建议保持单一构建。
- 导入/删除配置后若出现异常，先强制停止应用再重连，让两个进程重新加载。

## 故障排查

| 现象 | 原因与处理 |
|------|------------|
| 连接卡在 `Building configuration…` | 引擎找不到 profile（索引与文件不一致）。强制停止应用后重连；确认已打 `profile-list-sync.patch` |
| 日志 `Used x 101 tries to get current version (-1/N)` + NPE | 同上，profile 未加载，见上一行 |
| `remote-srv: SRV query failed`（C 层） | Android Bionic `res_query` 不可用，正常；实际由 Java 层解析 |
| `SRV resolution failed` | 当前网络 DNS 不可达或无 SRV 记录，检查 `_service._proto.name` 拼写与 DNS 服务器 |
| 认证阶段 `AUTH_FAILED` | 链路已通（TCP + TLS 成功），账号密码错误或为空 |
| `INSTALL_FAILED_TEST_ONLY` | 构建被注入 ABI 时会产生 testOnly 包，改为直接 `assembleUiOvpn2Debug` 或 `adb install -t` |
| 部分国产系统安装提示受限 | 需在系统里允许通过 USB 安装，或手动确认安装 |

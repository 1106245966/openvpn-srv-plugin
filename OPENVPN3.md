# OpenVPN 3 remote-srv 支持

为 OpenVPN 3 Core 增加 DNS SRV 自动发现：在配置中使用 `remote-srv _service._proto.name`，客户端在建连前查询 SRV，按 RFC 2782 选出一条记录，再把选中的 target/port 交给原有的 TCP、UDP、DCO 流程。

补丁文件：[openvpn3-core-remote-srv.patch](openvpn3-core-remote-srv.patch)

- 基线：`0c1ce172fe918b05463d71e682a0c2f2d8fe7899`（OpenVPN 3 Core master，2026-11-04）
- 规模：6 个文件，+885 / -42

## 与 OpenVPN 2 方案的差异

OpenVPN 2 有 `openvpn.exe` 这个进程边界，可以用 C shim 换 exe、改写配置后转发。OpenVPN 3 Core 是 C++ 库，被静态编译进宿主程序，没有可替换的独立文件，只能从源码打补丁后重新编译。

| 客户端 | 底层 | 本补丁 |
|--------|------|--------|
| 官方 OpenVPN Connect（Windows / macOS / Android / iOS） | OpenVPN 3 Core，闭源签名 | 不可用 |
| openvpn3-linux（`openvpn3` 命令行） | Core 子模块源码 | 重编 `openvpn3-service-client` 后可用 |
| `test/ovpncli`（Core 自带命令行客户端） | Core 源码 | 重编后可用 |
| 自建 App / 自己的 Core 集成 | Core 源码 | 重编后可用 |
| OpenVPN 2 客户端（OpenVPN GUI、ics-openvpn） | OpenVPN 2.x C 源码 | 不适用；桌面端用 C shim（见 [README.md](README.md)），Android 用 [src/android/remote-srv-android.patch](src/android/remote-srv-android.patch) |

## 配置语法

```text
client
dev tun
remote-srv _openvpn._udp.vpn.example.com
ca ca.crt
cert client.crt
key client.key
```

也可以写在 `<connection>` 块里，和其他 `remote` 条目混排：

```text
remote plain.example.com 1194 udp
remote-srv _openvpn._tcp.vpn.example.com

<connection>
  proto udp6
  remote-srv _openvpn._udp.vpn.example.com
</connection>
```

规则：

- 参数是完整 SRV 查询名，形如 `_service._proto.name`，只接受一个参数。
- 协议由名称中的 `_tcp` / `_udp` 决定；写 `_sctp` 等会在解析配置时报 `option_error`。
- 端口取自 SRV 记录的 port 字段，不使用配置里的默认端口。
- `remote-srv` 与 `remote` 保持配置中的先后顺序。

## 行为说明

**选路**：只在最低 priority 组内选择；组内按 weight 加权随机，权重全为零时等概率。忽略 target 为 `.`、target 为空、port 为 0 的记录。随机数由 I/O 线程取，DNS 工作线程只负责返回记录，避免线程不安全的随机源被跨线程使用。

**两阶段解析**：SRV 只解析出 target 主机名和端口，target 的 A/AAAA 仍交给原有传输层（`AsyncResolvableTCP/UDP`），因此 TCP、UDP、HTTP 代理、DCO、external transport 的既有逻辑不变。

**缓存与重连**：

- 未启用 remote cache（默认）：每次新建 Client 前重新查 SRV、重新选路。
- 启用 remote cache（`tunPersist` / `--remote-cache-lifetime`）：沿用原有缓存生命周期，在缓存有效期内复用已选中的 target 与地址。
- relay 重连沿用既有连接，不重新选路。

**失败处理**：查询失败或没有可用记录时记录 `Error::RESOLVE_ERROR`，走原有 `queue_restart()` 的延迟重连（默认 2 秒），不做同步递归重试。

**override 优先级**：命令行 `--server` 会清除 SRV 状态、退化为普通 remote；`--port` 覆盖 SRV 记录的端口。

**`remote-random-hostname`**：不会作用于 SRV 查询名，也不会给 SRV target 加随机前缀。

## 补丁内容

| 文件 | 改动 |
|------|------|
| `openvpn/client/srvresolve.hpp` | 新增。SRV 数据类型、协议名解析、RFC 2782 选路、DNS 报文解析、平台查询（Windows `DnsQuery_W` / POSIX `res_query`）、异步封装 |
| `openvpn/client/remotelist.hpp` | 新增 `remote-srv` 指令与 `Item` 的 SRV 状态；`actual_host()` / `actual_port()` 优先返回 SRV 结果；`BulkResolve` 支持先 SRV 再 A/AAAA |
| `openvpn/client/cliconnect.hpp` | `new_client()` 拆分：推进 remote → 必要时异步 SRV → `start_client()`；失败走 `queue_restart()`；停止、暂停、重连时取消未完成查询 |
| `openvpn/client/cliopt.hpp` | 新增 `remote_list_ptr()` 访问器 |
| `cmake/findcoredeps.cmake` | Windows 增加 `dnsapi.lib`，UNIX 增加 `resolv` |
| `test/unittests/test_remotelist.cpp` | 新增 5 个测试：配置解析、非法参数、priority/weight 选路、压缩指针解析、SRV → A/AAAA 全链路 |

异步实现说明：系统 DNS API 是同步的，因此放在 detached 工作线程执行，完成后通过 `openvpn_io::post` 回到 I/O 线程；取消时只 detach，不阻塞等待不可中断的系统调用，与上游 `AsyncResolvable` 的语义一致。

## 应用补丁

**当前基线**（补丁生成时对应的提交）：

```bash
cd openvpn3-core
git checkout 0c1ce172fe918b05463d71e682a0c2f2d8fe7899
git apply /path/to/openvpn3-core-remote-srv.patch
```

**其他版本**：补丁是单基线 diff，直接 `git apply` 在发布版本上会因上下文不同而失败。使用带完整历史的仓库做三路合并：

```bash
git clone https://github.com/OpenVPN/openvpn3.git
cd openvpn3
git checkout release/3.10.6
git apply --3way /path/to/openvpn3-core-remote-srv.patch
```

实测结果：

| 目标 | `git apply` | `git apply --3way` |
|------|-------------|--------------------|
| master @ `0c1ce172` | 通过 | 通过 |
| release/3.10.6 | 失败 | 干净合并 |
| release/3.9 | 失败 | 干净合并 |
| release/3.8.5 | 失败 | `remotelist.hpp` 冲突，需手工处理 |
| release/3.7.2 | 失败 | 未验证 |

`--3way` 需要仓库中存在补丁基线对应的对象，浅克隆会失败。

## 构建

依赖与上游一致（Asio、fmt、lz4、OpenSSL、jsoncpp、xxHash、tap-windows6），仓库中的 `deps/vcpkg_manifests/` 已提供对应清单。

Windows：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

补丁已把 `dnsapi.lib` 加入 Windows 链接列表；MinGW 构建同样由 `cmake/findcoredeps.cmake` 处理。

Linux：

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

补丁已把 `resolv` 加入 UNIX 链接列表。

Android / iOS：按宿主工程原有方式编译 Core（例如 ics-openvpn 的 core 子模块），补丁的 POSIX 分支走 `res_query`。

## 验证情况

已完成（Windows，MinGW GCC 11.2.0，C++20）：

- 新增头文件、`remotelist.hpp`、测试编译通过，`-Wall -Wextra` 无警告。
- 19 项 `RemoteList.*` / `SRVResolve.*` 测试全部通过（含全部原有 RemoteList 回归用例）。
- 补丁在干净基线克隆上 `git apply --check` 通过；实际应用后重新配置、编译、复跑测试仍全部通过。
- `git diff --check` 无空白错误。

未验证：

- `cliconnect.hpp` 只做了针对接入点的语法检查（基类初始化、`override` 签名、RemoteList 调用链），未在完整 Core 中编译（本机缺少 OpenSSL、jsoncpp、lz4、xxhash）。
- POSIX `res_query` 分支未编译（本机无 Linux 环境）；Apple / Android 上 `-lresolv` 的可用性未实测。
- 未做真实网络下的 SRV 解析与重连运行验证。
- 未构建 `ovpncli` 成品。

## 已知限制

- 补丁绑定单一基线提交，跨版本需要 `--3way` 或手工调整。
- 只能用于可自行编译的客户端；官方 OpenVPN Connect 的现成安装包无法使用。
- SRV 查询在 detach 线程中不可中断，进程退出时会直接放弃该线程。

# OpenVPN remote-srv 支持

为 OpenVPN 客户端增加 DNS SRV 记录解析能力：连接前通过 `remote-srv` 指令自动发现服务器地址与端口，并按 RFC 2782 的 priority/weight 规则选路。

当前实现统一为 C：

| 端 | 方案 | 特点 |
|----|------|------|
| Windows / Linux | C 透明包装 shim（`src/shim/`） | 单文件可执行，无运行时依赖，跨任意 OpenVPN 版本，升级只替换 1 个文件 |
| Android | ics-openvpn 源码补丁（C 引擎 + Java 应用层） | 真内置 `remote-srv` 指令；Android 上 `res_query` 不可用，由 Java 层兜底解析，需整包重编 APK |

## 目录结构

```
openvpn-plugin/
├── bin/                    # 编译产物（Windows / Linux / Android）
│   ├── openvpn.exe         # Windows shim，约 134 KB
│   ├── openvpn             # Linux x86_64（musl 静态，约 1.6 MB）
│   ├── openvpn-x86_64      # Linux x86_64 架构变体，约 1.6 MB
│   ├── openvpn-aarch64     # Linux AArch64 / ARM64 架构变体，约 1.7 MB
│   ├── openvpn-armv7       # Linux ARMv7 hard-float 架构变体，约 1.4 MB
│   ├── ics-openvpn-0.7.64-remote-srv-arm64-v8a-debug.apk   # Android（OpenVPN 2），约 30 MB
│   └── ics-openvpn-0.7.64-srv-ovpn3-arm64-v8a-debug.apk    # Android（OpenVPN 3），约 22 MB
├── src/
│   ├── shim/               # Windows / Linux 共用的 C shim 源码
│   │   ├── main.c          # 入口：解析 --config、处理 remote-srv、启动真 openvpn
│   │   ├── srv.c           # SRV 查询与 RFC 2782 选路（POSIX res_query / Windows DnsQuery_W）
│   │   └── srv.h
│   ├── windows/README.md   # Windows 端要点（编译/安装见根 README）
│   ├── linux/README.md     # Linux 端要点（含多架构交叉编译）
│   └── android/            # Android（OpenVPN 2 / ics-openvpn）归档
│       ├── ANDROID.md      # 方案说明：架构、补丁、编译、实测、排障
│       ├── remote-srv-android.patch    # C 引擎 + Java 应用层
│       └── profile-list-sync.patch     # 配置列表跨进程同步修复
├── openvpn3-core-remote-srv.patch  # OpenVPN 3 Core 补丁（详见 OPENVPN3.md）
├── OPENVPN3.md             # OpenVPN 3 Core 方案说明
└── openvpn-upstream/       # 上游 OpenVPN 源码（生成/对照补丁用，可删）
```

## remote-srv 指令语法

在 OpenVPN 配置文件中使用：

```text
client
dev tun
remote-srv _openvpn._udp.vpn.example.com
ca ca.crt
cert client.crt
key client.key
```

- 参数是完整的 SRV 查询名，形如 `_service._proto.name`。
- 协议由名称中的 `_tcp` / `_udp` 决定，端口取 SRV 记录中的端口。
- 查询结果按 priority（升序）分组，在最低优先级组内按 weight 加权随机选出 **1 条**记录，生成一行 `remote <target> <port> <proto>`；目标主机名交给 OpenVPN 自行解析。

---

## 桌面端 C shim（Windows / Linux）

### 工作原理

shim 是一个透明包装器，拦截对 openvpn 的调用：

1. 解析命令行中的 `--config <file>`（兼容 `--config=file`）。
2. 若配置含 `remote-srv`，查询 SRV 并生成临时配置，把该行替换为 `remote <target> <port> <proto>`，同时把 `--config` 指向临时配置。
3. 以子进程方式启动真正的 openvpn，透传全部参数与标准输入/输出/错误，等待退出并回传退出码。
4. 若配置不含 `remote-srv`（或命令行没有 `--config`），不生成临时配置，参数原样透传；若生成了临时配置，进程退出时删除。

**定位真 openvpn 的优先级**：环境变量 `OVPN_REAL_OPENVPN` → shim 同目录的 `openvpn-real.exe`（Windows）/ `openvpn.real`（Linux）。

### Windows 兼容性处理

OpenVPN GUI 实际通过 interactive service 启动 `openvpn.exe`，shim 夹在中间需要保持句柄语义：

- 手动 `CreateProcessW`，`bInheritHandles=TRUE` 全量继承可继承句柄，使 `--msg-channel` 句柄值在子进程中保持不变；stdout/stderr 指向同一个句柄。
- `STARTF_USESTDHANDLES` 直接透传 service 传给 shim 的标准句柄。
- **管理端口由 shim 自己监听**，再代理到 `openvpn-real.exe` 的内部端口：OpenVPN GUI 连接后会校验监听进程的镜像必须是 `bin\openvpn.exe`，如果让 `openvpn-real.exe` 直接监听，GUI 会判定异常、断开并终止连接。
- 保留 `--management ... stdin`：GUI 通过 interactive service 的 stdin 管道下发随机管理密码，shim 原样把 stdin 句柄传给子进程，密码与 banner 都经代理通道往返。
- `CREATE_NO_WINDOW` 启动子进程，并继承 shim 的进程优先级，与 service 直接启动的行为一致。
- 诊断日志写入当前工作目录的 `ovpn-srv-c.log`（记录 SRV 解析成功/失败原因、exe 路径、命令行、句柄值、CreateProcess 结果、管理端口代理映射）。在启动真 openvpn **之前**失败时，错误还会追加写入 GUI 传进来的 `--log` 文件，这样 OpenVPN GUI 的日志窗口能看到原因，而不是一片空白。

### Windows 安装

```powershell
# 1. 真 openvpn 改名 openvpn-real.exe，shim 顶替它的位置
#    （C:\Program Files 需要管理员权限，普通 PowerShell 会报“拒绝访问”）
Rename-Item "C:\Program Files\OpenVPN\bin\openvpn.exe" "openvpn-real.exe"
Copy-Item bin\openvpn.exe "C:\Program Files\OpenVPN\bin\openvpn.exe"

# 2. 配置放 C:\Program Files\OpenVPN\config\，用 OpenVPN GUI 照常导入连接

# 3. 升级：只替换 bin\openvpn.exe 这一个文件
```

> 覆盖 `C:\Program Files` 下的文件**必须以管理员身份运行** PowerShell（右键 → 以管理员身份运行），或用提权命令：
>
> ```powershell
> # 非管理员会话下提权执行复制（会弹出 UAC 确认）
> Start-Process powershell -Verb RunAs -ArgumentList '-Command', `
>   'Copy-Item "<仓库目录>\bin\openvpn.exe" "C:\Program Files\OpenVPN\bin\openvpn.exe" -Force'
> ```
>
> 安装后验证 shim 已就位：
>
> ```powershell
> # 真 openvpn 应已改名，shim 顶替原位置
> Get-ChildItem "C:\Program Files\OpenVPN\bin\openvpn*.exe" | Select-Object Name, Length
> #   openvpn.exe        136863   <- shim
> #   openvpn-real.exe  1077680   <- 真 openvpn
>
> # 透传冒烟：shim 调用真 openvpn 输出版本
> & "C:\Program Files\OpenVPN\bin\openvpn.exe" --version
> ```

> OpenVPN GUI 只是壳，内部用 `--config` 调用 `openvpn.exe`，换掉 exe 后 GUI 自动走 shim。TAP/Wintun 驱动沿用原有安装，无需改动。

### Linux 安装

```bash
# 1. 真 openvpn 改名 .real，shim 顶替它的位置（先按“编译”章节生成 bin/openvpn）
sudo mv /usr/local/sbin/openvpn /usr/local/sbin/openvpn.real
sudo cp bin/openvpn /usr/local/sbin/openvpn
sudo chmod +x /usr/local/sbin/openvpn

# 2. 启动（照常使用）
sudo openvpn --config /etc/openvpn/client/client.ovpn

# 3. 升级：只替换 /usr/local/sbin/openvpn 这一个文件
```

Linux 侧为 `fork` + `execvp`，透传标准输入/输出/错误并回传退出码。

**架构选型**：`bin/openvpn` 为 x86_64 版本。ARM 设备请选用对应架构的产物：

| 设备/架构 | 产物 |
|-----------|------|
| x86_64 服务器/PC | `bin/openvpn` 或 `bin/openvpn-x86_64` |
| ARM64 服务器 / 树莓派 4+ / 飞腾等 | `bin/openvpn-aarch64` |
| 树莓派 2/3 / ARMv7 硬浮点设备 | `bin/openvpn-armv7` |

均为 musl 静态链接，无需在目标机安装任何运行时依赖。

---

## Android 端（ics-openvpn 补丁）

Android 没有独立的 openvpn 可执行文件调用链，需把补丁应用到 [ics-openvpn](https://github.com/schwabe/ics-openvpn) 源码后整包重编 APK。

已验证版本：ics-openvpn `v0.7.64`（ovpn2 风味，底层为 OpenVPN `2.7_rc6` 的 fork），产物见 `bin/ics-openvpn-0.7.64-remote-srv-arm64-v8a-debug.apk`。

| 补丁 | 作用 |
|------|------|
| `src/android/remote-srv-android.patch` | 识别并解析 `remote-srv`：C 引擎层 + Java 应用层 |
| `src/android/profile-list-sync.patch` | 修复配置列表在两个进程间不同步的问题 |

```bash
git clone --recursive https://github.com/schwabe/ics-openvpn.git
cd ics-openvpn
git apply /path/to/src/android/remote-srv-android.patch
git apply /path/to/src/android/profile-list-sync.patch
# 用 Android Studio 打开工程，Sync + Build 出 APK 安装
```

补丁路径相对仓库根目录，已包含 `main/src/main/cpp/openvpn/...`，无需进入子模块单独打补丁。

> Android 13+ 的 Bionic `res_query()` 依赖已废弃的 `net.dns1`，SRV 查询不可用；Android 实际由 Java 层 `SrvResolver` 解析，再把 `remote-srv` 改写为普通 `remote`。C 层实现保留给桌面端/自编 openvpn。

架构说明、补丁改动点、编译与使用、实测记录、已知限制与故障排查见 [src/android/ANDROID.md](src/android/ANDROID.md)。

---

## 编译与运行验证

### C shim

```bash
# 在仓库根目录执行，编译产物统一输出到 bin/

# Windows（MinGW）
gcc -O2 -Wall -Wextra src/shim/main.c src/shim/srv.c -o bin/openvpn.exe -lws2_32 -ldnsapi

# Windows（MSVC）
cl /O2 /Fo:bin/ src/shim/main.c src/shim/srv.c /Fe:bin/openvpn.exe /link ws2_32.lib dnsapi.lib

# Linux
gcc -O2 -Wall -Wextra src/shim/main.c src/shim/srv.c -o bin/openvpn -lresolv

# Linux 多架构交叉编译（用 zig cc，从 Windows / macOS / Linux 均可一键产出所有架构的静态二进制）
# musl 静态链接，产物无运行时依赖，拷到任意 Linux 发行版即可运行
zig cc -target x86_64-linux-musl    -O2 -Wall -Wextra -D_GNU_SOURCE src/shim/main.c src/shim/srv.c -o bin/openvpn-x86_64
zig cc -target aarch64-linux-musl   -O2 -Wall -Wextra -D_GNU_SOURCE src/shim/main.c src/shim/srv.c -o bin/openvpn-aarch64
zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -O2 -Wall -Wextra -D_GNU_SOURCE src/shim/main.c src/shim/srv.c -o bin/openvpn-armv7
# x86_64 同时作为默认 bin/openvpn
cp bin/openvpn-x86_64 bin/openvpn
```

编译检查：

- `-Wall -Wextra` 下应无警告。
- 若报 `undefined reference to '__imp__DnsQuery_W'`，说明没链接 DNS API：MinGW 加 `-ldnsapi`，MSVC 加 `dnsapi.lib`。
- Windows 版还用到 Winsock（管理端口代理）：MinGW 加 `-lws2_32`，MSVC 加 `ws2_32.lib`。
- 产物可直接用 `--version` 之类的参数做一次透传冒烟测试（需要对应的 `openvpn-real.exe` / `openvpn.real` 已就位）。
- **不要**把 `OVPN_REAL_OPENVPN` 指向 shim 自身，否则 shim 会递归调用自己。

#### 编译环境与产物校验

本机验证环境：

```
MinGW.org GCC Build-2 9.2.0（32-bit）
命令：gcc -O2 -Wall -Wextra src/shim/main.c src/shim/srv.c -o bin/openvpn.exe -lws2_32 -ldnsapi
产物：bin/openvpn.exe，约 134 KB（136,863 字节）
```

要点：

- **32-bit shim 可正常启动 64-bit `openvpn-real.exe`**：`CreateProcess` 不要求父子进程同架构，句柄继承与端口代理均不受位数影响。
- 编译应在 `-Wall -Wextra` 下无警告；如有 `unused parameter` 之类的告警，说明源码与当前编译器版本不匹配，需检查。
- 产物校验（可选）：

```powershell
# 查看产物大小与时间戳，确认是本次编译产出
Get-Item bin\openvpn.exe | Select-Object Name, Length, LastWriteTime

# 确认依赖的系统 DLL（应只含 ws2_32、dnsapi、msvcrt 等系统库，无第三方依赖）
# 可用 Dependencies / dumpbin /depends 等工具查看
```

Linux 多架构交叉编译环境（zig cc）：

```
Zig 0.17.1（Windows x86_64 宿主）
命令：zig cc -target <arch>-linux-musl -O2 -Wall -Wextra -D_GNU_SOURCE src/shim/main.c src/shim/srv.c -o bin/openvpn-<arch>
产物：
  bin/openvpn-x86_64   ELF 64-bit LE, e_machine=0x3E (x86_64),  ~1.6 MB, 静态链接
  bin/openvpn-aarch64  ELF 64-bit LE, e_machine=0xB7 (AArch64), ~1.7 MB, 静态链接
  bin/openvpn-armv7    ELF 32-bit LE, e_machine=0x28 (ARM),     ~1.4 MB, 静态链接
```

要点：

- **musl 静态链接**：zig cc 目标为 `*-linux-musl` 时默认静态链接，产物不依赖目标机 glibc 版本，也无需 `-lresolv`（`res_query` 已内置于 musl libc）。
- **跨宿主编译**：同一条 `zig cc -target` 命令在 Windows / macOS / Linux 宿主上产出一致的 Linux 二进制，无需为每个架构准备独立工具链。
- `bin/openvpn` 是 `bin/openvpn-x86_64` 的副本，与「Linux 安装」章节的路径一致；ARM 设备请改用对应架构的 `openvpn-aarch64` / `openvpn-armv7`。
- 产物校验（ELF 头部与静态链接确认）：

```powershell
# 查看各产物的 ELF 架构与大小
Get-ChildItem bin\openvpn-* | ForEach-Object {
  $b = [IO.File]::ReadAllBytes($_.FullName)
  $em = $b[18] -bor ($b[19] -shl 8)
  $arch = @{0x3E='x86_64'; 0xB7='AArch64'; 0x28='ARM'}[$em]
  [PSCustomObject]@{ Name=$_.Name; Arch=$arch; SizeKB=[math]::Round($_.Length/1KB,1) }
} | Format-Table -AutoSize

# 确认无动态链接器依赖（静态）：字符串中不含 ld-linux / ld-musl
Select-String -Path bin\openvpn-aarch64 -Pattern 'ld-linux|ld-musl' -Quiet   # 应返回 False
```

### Android APK

```bash
# 应用两个补丁后，在 ics-openvpn 根目录执行
./gradlew :main:assembleUiOvpn2Debug
# 产物：main/build/outputs/apk/uiOvpn2/debug/main-ui-ovpn2-arm64-v8a-debug.apk
```

环境要求、ABI 分包建议与设备实测结果见 [src/android/ANDROID.md](src/android/ANDROID.md)。

### 运行验证（不需要真的 openvpn）

用一个小桩程序代替真正的 openvpn：打印收到的参数，并输出 `--config` 指向的配置内容，便于确认 shim 改写了什么。

```c
/* stub.c：参数/配置回显桩 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i;
    int fake_exit = 0;
    const char *cfg = NULL;
    for (i = 0; i < argc; i++)
    {
        printf("ARG[%d]=%s\n", i, argv[i]);
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
        {
            cfg = argv[i + 1];
        }
        else if (!strncmp(argv[i], "--config=", 9))
        {
            cfg = argv[i] + 9;
        }
        else if (!strncmp(argv[i], "--fake-exit=", 12))
        {
            fake_exit = atoi(argv[i] + 12);
        }
    }
    if (cfg)
    {
        char buf[2048];
        size_t n;
        FILE *fp = fopen(cfg, "rb");
        while (fp && (n = fread(buf, 1, sizeof(buf), fp)) > 0)
        {
            fwrite(buf, 1, n, stdout);
        }
        if (fp)
        {
            fclose(fp);
        }
    }
    return fake_exit;
}
```

验证步骤：

```powershell
# 1. 编译 shim 与桩程序（先把上面的 stub.c 保存为 %TEMP%\stub.c）
# 在仓库根目录执行
gcc -O2 -Wall -Wextra src/shim/main.c src/shim/srv.c -o "$env:TEMP\ovpn-srv-test.exe" -lws2_32 -ldnsapi
gcc -O2 "$env:TEMP\stub.c" -o "$env:TEMP\fake-openvpn.exe"

# 2. 准备测试配置：remote-srv 指向一个真实存在的 SRV 记录
@"
client
dev tun
remote-srv _sip._udp.sip2sip.info
ca ca.crt
"@ | Set-Content "$env:TEMP\srv-test.ovpn" -Encoding ASCII

# 3. 让 shim 把桩程序当成真正的 openvpn
$env:OVPN_REAL_OPENVPN = "$env:TEMP\fake-openvpn.exe"
& "$env:TEMP\ovpn-srv-test.exe" --config "$env:TEMP\srv-test.ovpn" --verb 3
"exit=$LASTEXITCODE"
```

期望结果：

- shim 传给桩程序的 `--config` 已指向临时文件（如 `%TEMP%\ovpXXXX.tmp`）。
- 临时配置中 `remote-srv ...` 被替换为一行 `remote <target> <port> <proto>`（实测：`remote proxy.sipthor.net 5060 udp`），其余行原样保留。
- shim 退出后临时文件已删除（`Test-Path` 返回 `False`）。

再补三项回归检查：

```powershell
# 准备两份配置：一份无 remote-srv，一份 SRV 名不存在
@"
client
dev tun
remote vpn.example.com 1194
"@ | Set-Content "$env:TEMP\plain.ovpn" -Encoding ASCII
"client`nremote-srv _openvpn._udp.invalid.example.com`n" | Set-Content "$env:TEMP\bad-srv.ovpn" -Encoding ASCII

# 透传：配置不含 remote-srv 时，--config 应仍指向原文件
& "$env:TEMP\ovpn-srv-test.exe" --config="$env:TEMP\plain.ovpn"

# 失败路径：SRV 查询失败时应打印错误并以 1 退出
& "$env:TEMP\ovpn-srv-test.exe" --config "$env:TEMP\bad-srv.ovpn"; "exit=$LASTEXITCODE"

# 退出码透传：桩程序返回 7 时 shim 也应返回 7
& "$env:TEMP\ovpn-srv-test.exe" --config "$env:TEMP\srv-test.ovpn" --fake-exit=7; "exit=$LASTEXITCODE"
```

Linux 下把 `OVPN_REAL_OPENVPN` 指向本地编译的桩程序（`gcc stub.c -o fake-openvpn`），命令形式相同；装到真实环境后还可用 `--verb 4` 从 OpenVPN 日志确认实际连接的 `remote <target> <port>`。

### GUI 端到端验证（Windows，真实 OpenVPN）

stub 只验证 shim 的参数改写逻辑；要确认 interactive service、管理端口代理、句柄透传整条链路正常，需用真实 OpenVPN GUI 跑一次连接。

前置条件：

- 已按 [Windows 安装](#windows-安装) 把 shim 放到 `C:\Program Files\OpenVPN\bin\openvpn.exe`，真 openvpn 改名为 `openvpn-real.exe`。
- 有一份可正常连接的配置（先**不含** `remote-srv`，确认基础链路通了再测 SRV）。

验证步骤：

```powershell
# 1. 用 OpenVPN GUI 触发一次连接（配置名按实际替换）
& "C:\Program Files\OpenVPN\bin\openvpn-gui.exe" --connect OpenVPN-Client

# 2. 连接建立后，检查管理端口监听者必须是 shim（openvpn.exe），而非 openvpn-real.exe
#    GUI 会校验：监听进程镜像路径必须等于 HKLM\SOFTWARE\OpenVPN\exe_path
Get-NetTCPConnection -State Listen | Where-Object { $_.LocalAddress -eq '127.0.0.1' } |
  ForEach-Object {
    $p = Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue
    [PSCustomObject]@{ Port = $_.LocalPort; PID = $_.OwningProcess; Image = $p.Path }
  } | Format-Table -AutoSize
# 期望：外部管理端口（如 25341）的 Image = C:\Program Files\OpenVPN\bin\openvpn.exe（shim）
#       内部端口（如 25342）的 Image = ...\openvpn-real.exe（子进程）

# 3. 查看 shim 诊断日志，确认代理映射与 cmdline（含 stdin）
Get-Content "C:\Program Files\OpenVPN\config\OpenVPN-Client\ovpn-srv-c.log" -Tail 30
# 期望看到：management proxy: <ext_port> -> <int_port>
#           以及子进程 cmdline 中保留了 --management ... stdin

# 4. 查看 OpenVPN 连接日志，确认连接成功
Get-Content "C:\Program Files\OpenVPN\log\OpenVPN-Client.log" -Tail 20
# 期望看到：Initialization Sequence Completed
#           MANAGEMENT: >STATE:...,CONNECTED,SUCCESS,<虚拟IP>,<服务器IP>,<端口>,...
```

判定标准：

- 管理端口监听者是 `openvpn.exe`（shim），**不是** `openvpn-real.exe`，否则 GUI 会判定镜像不符而断开（表现为闪退）。
- 日志出现 `Initialization Sequence Completed` 与 `CONNECTED,SUCCESS`。
- shim 日志中 `management proxy: <ext> -> <int>` 映射正常，子进程 cmdline 保留 `stdin`。

带 `remote-srv` 的配置验证：把配置中的 `remote <host> <port>` 改为 `remote-srv <srv-name>`，重复上述步骤，在连接日志中确认实际连接的 `<target> <port>` 来自 SRV 解析结果。

---

## 验证情况

本机验证（Windows，MinGW.org GCC 9.2.0，`-O2 -Wall -Wextra`）：

- 编译通过，无警告。
- 真实 SRV 查询通过：`_sip._udp.sip2sip.info` → `remote proxy.sipthor.net 5060 udp`，其余配置行与命令行参数原样保留。
- 不含 `remote-srv` 时透传正常（`--config` / `--config=` 两种写法均验证）。
- 与真实 OpenVPN 2.7.8（Windows）透传验证通过：`openvpn.exe --version` 正常输出，退出码 0。
- 临时配置在 shim 退出后正确删除；SRV 解析失败时以 1 退出；子进程退出码正确回传（7 → 7）。
- GUI 场景问题已定位并修复：OpenVPN GUI 会校验管理端口监听者镜像必须是 `bin\openvpn.exe`，此前由 `openvpn-real.exe` 监听会被判定异常并立即断开（表现为 GUI 闪退）。现由 shim 监听 GUI 端口（实测 listener PID 与 shim PID 一致），再代理到子进程内部端口。
- 端到端验证通过（OpenVPN GUI 11.66 + OpenVPN 2.7.8 + interactive service）：`openvpn-gui --connect` 后管理端口 25341 由 shim 监听、25342 为子进程内部端口，连接成功并输出 `Initialization Sequence Completed` / `CONNECTED,SUCCESS`。
- Linux 多架构交叉编译通过（Windows 宿主 + Zig 0.17.1）：用 `zig cc -target <arch>-linux-musl` 产出 x86_64 / AArch64 / ARMv7 三个 ELF 静态二进制（musl 静态链接，无运行时依赖），ELF 头部校验 `e_machine` 分别为 `0x3E` / `0xB7` / `0x28`，`-Wall -Wextra` 无警告；目标机运行验证需在对应架构的 Linux 上执行。
- Android 端（remote-srv + 配置列表修复）已完成整包编译与设备实测：SRV 解析出目标地址、TCP 连接 `CONNECTED,SUCCESS`、隧道 `ping` 0% 丢包；补丁从原始 `v0.7.64` 可完整复现。详见 [src/android/ANDROID.md](src/android/ANDROID.md#实测记录)。

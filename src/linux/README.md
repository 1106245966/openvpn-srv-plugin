# Linux 端（C shim）

Linux 通过替换 `openvpn` 的透明 shim 实现 `remote-srv`：

- 源码：`src/shim/`（`main.c`、`srv.c`、`srv.h`，与 Windows 共用）
- 产物：`bin/openvpn`（x86_64）、`bin/openvpn-aarch64`、`bin/openvpn-armv7`（musl 静态链接）
- 编译（含 zig cc 多架构交叉编译）、安装步骤：见仓库根目录 [README.md](../../README.md)

```bash
# 本机编译
gcc -O2 -Wall -Wextra src/shim/main.c src/shim/srv.c -o bin/openvpn -lresolv

# 交叉编译（musl 静态）
zig cc -target x86_64-linux-musl -O2 -Wall -Wextra -D_GNU_SOURCE src/shim/main.c src/shim/srv.c -o bin/openvpn-x86_64
```

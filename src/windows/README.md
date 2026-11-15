# Windows 端（C shim）

Windows 通过替换 `openvpn.exe` 的透明 shim 实现 `remote-srv`：

- 源码：`src/shim/`（`main.c`、`srv.c`、`srv.h`，与 Linux 共用）
- 产物：`bin/openvpn.exe`（真 openvpn 改名 `openvpn-real.exe`，shim 顶替其位置）
- 编译命令、安装步骤、GUI 端到端验证：见仓库根目录 [README.md](../../README.md)

```powershell
gcc -O2 -Wall -Wextra src/shim/main.c src/shim/srv.c -o bin/openvpn.exe -lws2_32 -ldnsapi
```

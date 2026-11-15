/*
 * main.c - OpenVPN remote-srv shim（跨平台 C 实现）
 *
 * 拦截 openvpn 调用，解析 --config 中的 remote-srv 指令：
 *   1. 若配置含 remote-srv，解析 SRV 并生成临时配置（remote-srv -> remote）
 *   2. 透传所有参数给真正的 openvpn（openvpn-real.exe / openvpn.real）
 *
 * Windows 上手动 CreateProcess，保持 stdout/stderr 指向同一个句柄，
 * 并透传 interactive service 的 --msg-channel 句柄。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
/* winsock2.h 必须在 windows.h 之前包含 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#define strdup _strdup

#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#endif

#include "srv.h"

/* 临时配置文件路径（进程退出前清理） */
static char *g_tmp_config = NULL;

/*
 * 写诊断日志到 shim 同目录的 ovpn-srv-c.log（便于排查 CreateProcess/句柄问题）。
 */
static void shim_log(const char *fmt, ...)
{
    FILE *fp = fopen("ovpn-srv-c.log", "a");
    if (fp)
    {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fputc('\n', fp);
        fclose(fp);
    }
}

/*
 * 在参数列表中查找 --x value 或 --x=value 的值，未找到返回 NULL。
 */
static const char *find_arg_value(int argc, char **argv, const char *opt)
{
    size_t olen = strlen(opt);
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], opt) == 0 && i + 1 < argc)
        {
            return argv[i + 1];
        }
        if (strncmp(argv[i], opt, olen) == 0 && argv[i][olen] == '=')
        {
            return argv[i] + olen + 1;
        }
    }
    return NULL;
}

/*
 * 报告致命错误：stderr + ovpn-srv-c.log + GUI 的 --log 文件。
 *
 * shim 在启动真 openvpn 之前失败时，openvpn 不会创建自己的日志文件，
 * GUI 的日志窗口就会一片空白；因此错误必须由 shim 直接写进 --log 指向的文件。
 */
static void shim_report(int argc, char **argv, const char *fmt, ...)
{
    char msg[1024];
    char stamp[32];
    va_list ap;
    const char *logpath = find_arg_value(argc, argv, "--log");
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (tmv)
    {
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S ", tmv);
    }
    else
    {
        stamp[0] = '\0';
    }

    fprintf(stderr, "ovpn-srv: %s\n", msg);
    shim_log("%s", msg);

    if (logpath && logpath[0])
    {
        FILE *fp = fopen(logpath, "a");
        if (fp)
        {
            fprintf(fp, "%sovpn-srv: %s\n", stamp, msg);
            fclose(fp);
        }
    }
}

/*
 * 在参数列表中查找 --config file 或 --config=file，返回文件路径下标与路径。
 */
static int find_config_arg(int argc, char **argv, char **path_out)
{
    int i;
    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
        {
            *path_out = argv[i + 1];
            return i;
        }
        if (strncmp(argv[i], "--config=", 9) == 0)
        {
            *path_out = argv[i] + 9;
            return i;
        }
    }
    *path_out = NULL;
    return -1;
}

/*
 * 定位真正的 openvpn 可执行文件。
 * 优先级：环境变量 OVPN_REAL_OPENVPN > shim 同目录下的 openvpn-real.exe / openvpn.real
 */
#ifdef _WIN32
/* 判断给定路径是否就是 shim 自身，防止 OVPN_REAL_OPENVPN 配错导致递归调用。 */
static int is_self_path(const char *path)
{
    char self[MAX_PATH];
    char full_path[MAX_PATH];
    char full_self[MAX_PATH];

    if (GetModuleFileNameA(NULL, self, (DWORD)sizeof(self)) == 0)
    {
        return 0;
    }
    if (GetFullPathNameA(path, (DWORD)sizeof(full_path), full_path, NULL) == 0)
    {
        return 0;
    }
    if (GetFullPathNameA(self, (DWORD)sizeof(full_self), full_self, NULL) == 0)
    {
        return 0;
    }
    return _stricmp(full_path, full_self) == 0;
}
#endif

static const char *find_real_openvpn(const char *argv0)
{
    static char buf[512];
    const char *env = getenv("OVPN_REAL_OPENVPN");
    const char *name;
    const char *slash;
    size_t dirlen;

#ifdef _WIN32
    if (env && env[0] && is_self_path(env))
    {
        shim_log("OVPN_REAL_OPENVPN 指向 shim 自身，已忽略: %s", env);
        env = NULL;
    }
#endif

    if (env && env[0])
    {
        return env;
    }

#ifdef _WIN32
    name = "openvpn-real.exe";

    /* 用 GetModuleFileName 获取 shim 自身的完整路径，替换文件名为 openvpn-real.exe，
     * 避免依赖 argv[0]（可能是相对名）与工作目录。 */
    if (GetModuleFileNameA(NULL, buf, (DWORD)sizeof(buf)) > 0)
    {
        const char *bslash = strrchr(buf, '\\');
        if (bslash)
        {
            size_t d = (size_t)(bslash - buf) + 1;
            if (d + strlen(name) + 1 <= sizeof(buf))
            {
                strcpy(buf + d, name);
                return buf;
            }
        }
    }
#else
    name = "openvpn.real";
#endif

    /* 回退：从 argv[0] 推导 shim 所在目录 */
    slash = strrchr(argv0, '/');
#ifdef _WIN32
    {
        const char *bslash = strrchr(argv0, '\\');
        if (!slash || (bslash && bslash > slash))
        {
            slash = bslash;
        }
    }
#endif
    if (slash)
    {
        dirlen = (size_t)(slash - argv0) + 1;
    }
    else
    {
        dirlen = 0;
    }

    if (dirlen + strlen(name) + 1 >= sizeof(buf))
    {
        return name;
    }
    if (dirlen > 0)
    {
        memcpy(buf, argv0, dirlen);
    }
    strcpy(buf + dirlen, name);
    return buf;
}

/*
 * 创建临时配置文件，返回可写 FILE* 与路径。
 */
static FILE *create_tmp_config(char *path_buf, size_t path_len)
{
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    char tmpfile[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, tmpdir))
    {
        strcpy(tmpdir, ".");
    }
    if (!GetTempFileNameA(tmpdir, "ovpn", 0, tmpfile))
    {
        return NULL;
    }
    if (strlen(tmpfile) >= path_len)
    {
        return NULL;
    }
    strcpy(path_buf, tmpfile);
    return fopen(path_buf, "wb");
#else
    char tmpl[] = "/tmp/ovpn-srv-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
    {
        return NULL;
    }
    if (strlen(tmpl) >= path_len)
    {
        close(fd);
        return NULL;
    }
    strcpy(path_buf, tmpl);
    return fdopen(fd, "wb");
#endif
}

/*
 * 读取整个文件到内存，返回 malloc 的缓冲区，*len 为长度。
 */
static char *read_file(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    long size;
    char *buf;

    if (!fp)
    {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0)
    {
        fclose(fp);
        return NULL;
    }

    buf = (char *)malloc((size_t)size + 1);
    if (!buf)
    {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size)
    {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[size] = '\0';
    fclose(fp);
    *len = (size_t)size;
    return buf;
}

/*
 * 判断一行是否为 remote-srv 指令，并提取 SRV 名称。
 * 支持 remote-srv _service._proto.name
 */
static int parse_remote_srv_line(const char *line, const char **srv_name)
{
    const char *p = line;
    /* 跳过前导空白 */
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (strncmp(p, "remote-srv", 10) != 0)
    {
        return 0;
    }
    p += 10;
    /* 需要空白分隔 */
    if (*p != ' ' && *p != '\t')
    {
        return 0;
    }
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (*p == '\0' || *p == '\n' || *p == '\r')
    {
        return 0;
    }
    *srv_name = p;
    return 1;
}

/*
 * 处理配置文件：若含 remote-srv，则解析 SRV 并生成临时配置，替换 --config。
 * 返回 0 成功（含无需处理的情况），-1 失败。
 */
static int process_remote_srv(int argc, char **argv)
{
    int cfg_idx;
    char *cfg_path = NULL;
    char *content;
    size_t content_len;
    char *out;
    size_t out_cap;
    size_t out_len;
    char tmp_path[512];
    FILE *tmp_fp;
    char *line;
    int has_srv = 0;

    cfg_idx = find_config_arg(argc, argv, &cfg_path);
    if (cfg_idx < 0 || !cfg_path)
    {
        return 0; /* 无 --config，直接透传 */
    }

    content = read_file(cfg_path, &content_len);
    if (!content)
    {
        shim_report(argc, argv, "读取配置 %s 失败", cfg_path);
        return -1;
    }

    /* 先生成输出缓冲区 */
    out_cap = content_len + 512;
    out = (char *)malloc(out_cap);
    if (!out)
    {
        free(content);
        return -1;
    }
    out_len = 0;
    out[0] = '\0';

    /* 逐行处理 */
    line = strtok(content, "\n");
    while (line)
    {
        size_t llen = strlen(line);
        const char *srv_name = NULL;
        /* 去掉 Windows 行尾的 \r */
        if (llen > 0 && line[llen - 1] == '\r')
        {
            line[llen - 1] = '\0';
            llen--;
        }
        if (parse_remote_srv_line(line, &srv_name))
        {
            char *host = NULL;
            int port = 0;
            int proto = 0;
            char errbuf[256] = "";
            if (srv_resolve(srv_name, &host, &port, &proto,
                            errbuf, sizeof(errbuf)) == 0 && host)
            {
                char newline[512];
                const char *protoname = proto ? "tcp" : "udp";
                has_srv = 1;
                snprintf(newline, sizeof(newline), "remote %s %d %s",
                         host, port, protoname);
                shim_log("SRV %s -> remote %s %d %s", srv_name, host, port, protoname);
                free(host);
                if (out_len + strlen(newline) + 1 >= out_cap)
                {
                    out_cap = out_len + strlen(newline) + 512;
                    out = (char *)realloc(out, out_cap);
                    if (!out)
                    {
                        free(content);
                        return -1;
                    }
                }
                strcpy(out + out_len, newline);
                out_len += strlen(newline);
            }
            else
            {
                shim_report(argc, argv, "SRV 解析失败: %s (%s)", srv_name,
                            errbuf[0] ? errbuf : "未知原因");
                free(content);
                free(out);
                return -1;
            }
        }
        else
        {
            /* 原样保留该行 */
            if (out_len + strlen(line) + 1 >= out_cap)
            {
                out_cap = out_len + strlen(line) + 512;
                out = (char *)realloc(out, out_cap);
                if (!out)
                {
                    free(content);
                    return -1;
                }
            }
            strcpy(out + out_len, line);
            out_len += strlen(line);
        }
        out[out_len++] = '\n';
        out[out_len] = '\0';
        line = strtok(NULL, "\n");
    }
    free(content);

    if (!has_srv)
    {
        free(out);
        return 0; /* 无 remote-srv，直接透传 */
    }

    /* 写临时配置 */
    tmp_fp = create_tmp_config(tmp_path, sizeof(tmp_path));
    if (!tmp_fp)
    {
        shim_report(argc, argv, "创建临时配置失败");
        free(out);
        return -1;
    }
    fwrite(out, 1, out_len, tmp_fp);
    fclose(tmp_fp);
    free(out);

    g_tmp_config = strdup(tmp_path);

    /* 替换 --config 参数指向临时配置 */
    if (strncmp(argv[cfg_idx], "--config=", 9) == 0)
    {
        size_t need = strlen(tmp_path) + 10;
        char *newarg = (char *)malloc(need);
        if (!newarg)
        {
            return -1;
        }
        snprintf(newarg, need, "--config=%s", tmp_path);
        argv[cfg_idx] = newarg;
    }
    else
    {
        argv[cfg_idx + 1] = g_tmp_config;
    }
    return 0;
}

#ifdef _WIN32

/*
 * Windows 管理接口代理
 *
 * OpenVPN GUI 在连接管理端口后会校验监听进程的镜像路径必须等于 o.exe_path
 * （即 ...\bin\openvpn.exe）。如果让 openvpn-real.exe 直接监听，校验失败，
 * GUI 会立即断开并终止连接。
 *
 * 因此由 shim 自己监听 GUI 指定的端口，再把流量转发给 openvpn-real.exe 的
 * 内部端口，使监听者保持为 shim（镜像路径与 o.exe_path 一致）。
 */

static volatile LONG g_mgmt_stop = 0;
static SOCKET g_mgmt_listen = INVALID_SOCKET;
static unsigned short g_mgmt_ext_port = 0;
static unsigned short g_mgmt_int_port = 0;
static HANDLE g_mgmt_thread = NULL;

static int mgmt_port_available(unsigned short port)
{
    SOCKET s;
    struct sockaddr_in addr;
    int ok = 0;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        return 0;
    }
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
    {
        ok = 1;
    }
    closesocket(s);
    return ok;
}

static unsigned short pick_internal_port(unsigned short base)
{
    int p;
    for (p = (int)base + 1; p <= (int)base + 100 && p < 65536; p++)
    {
        if (mgmt_port_available((unsigned short)p))
        {
            return (unsigned short)p;
        }
    }
    return 0;
}

static int send_all(SOCKET s, const char *buf, int len)
{
    int sent = 0;
    while (sent < len)
    {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0)
        {
            return 0;
        }
        sent += n;
    }
    return 1;
}

static void proxy_relay(SOCKET client, SOCKET backend)
{
    char buf[4096];

    while (!InterlockedCompareExchange(&g_mgmt_stop, 0, 0))
    {
        fd_set rf;
        struct timeval tv;
        int n;

        FD_ZERO(&rf);
        FD_SET(client, &rf);
        FD_SET(backend, &rf);
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        if (select(0, &rf, NULL, NULL, &tv) <= 0)
        {
            continue;
        }
        if (FD_ISSET(client, &rf))
        {
            n = recv(client, buf, (int)sizeof(buf), 0);
            if (n <= 0 || !send_all(backend, buf, n))
            {
                break;
            }
        }
        if (FD_ISSET(backend, &rf))
        {
            n = recv(backend, buf, (int)sizeof(buf), 0);
            if (n <= 0 || !send_all(client, buf, n))
            {
                break;
            }
        }
    }
    shutdown(client, SD_BOTH);
    shutdown(backend, SD_BOTH);
    closesocket(client);
    closesocket(backend);
}

static SOCKET connect_backend(unsigned short port)
{
    int i;
    for (i = 0; i < 100; i++)
    {
        SOCKET s;
        struct sockaddr_in addr;

        if (InterlockedCompareExchange(&g_mgmt_stop, 0, 0))
        {
            return INVALID_SOCKET;
        }
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
        {
            return INVALID_SOCKET;
        }
        ZeroMemory(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        {
            return s;
        }
        closesocket(s);
        Sleep(100);
    }
    return INVALID_SOCKET;
}

static DWORD WINAPI mgmt_proxy_thread(LPVOID param)
{
    (void)param;

    while (!InterlockedCompareExchange(&g_mgmt_stop, 0, 0))
    {
        fd_set rf;
        struct timeval tv;
        SOCKET client;
        SOCKET backend;

        FD_ZERO(&rf);
        FD_SET(g_mgmt_listen, &rf);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(0, &rf, NULL, NULL, &tv) <= 0 || !FD_ISSET(g_mgmt_listen, &rf))
        {
            continue;
        }
        client = accept(g_mgmt_listen, NULL, NULL);
        if (client == INVALID_SOCKET)
        {
            continue;
        }
        backend = connect_backend(g_mgmt_int_port);
        if (backend == INVALID_SOCKET)
        {
            closesocket(client);
            continue;
        }
        proxy_relay(client, backend);
    }
    return 0;
}

/*
 * 若命令行含 --management 127.0.0.1 <port>，创建 GUI 端口监听并改写子进程端口。
 * 返回 1 表示已启用代理。
 */
static int setup_mgmt_proxy(int argc, char **argv)
{
    int i;
    int idx = -1;
    int port;
    unsigned short internal_port;
    SOCKET s;
    struct sockaddr_in addr;
    BOOL yes = TRUE;
    static char internal_port_str[8];

    for (i = 1; i + 2 < argc; i++)
    {
        if (strcmp(argv[i], "--management") == 0)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0 || strcmp(argv[idx + 1], "127.0.0.1") != 0)
    {
        return 0;
    }
    port = atoi(argv[idx + 2]);
    if (port <= 0 || port > 65535)
    {
        return 0;
    }

    internal_port = pick_internal_port((unsigned short)port);
    if (!internal_port)
    {
        return 0;
    }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        return 0;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s, 4) != 0)
    {
        closesocket(s);
        return 0;
    }

    g_mgmt_listen = s;
    g_mgmt_ext_port = (unsigned short)port;
    g_mgmt_int_port = internal_port;

    snprintf(internal_port_str, sizeof(internal_port_str), "%u", (unsigned)internal_port);
    argv[idx + 2] = internal_port_str;

    shim_log("management proxy: %u -> %u", (unsigned)g_mgmt_ext_port,
             (unsigned)g_mgmt_int_port);
    return 1;
}

static void start_mgmt_proxy(void)
{
    if (g_mgmt_listen == INVALID_SOCKET)
    {
        return;
    }
    g_mgmt_thread = CreateThread(NULL, 0, mgmt_proxy_thread, NULL, 0, NULL);
    if (!g_mgmt_thread)
    {
        closesocket(g_mgmt_listen);
        g_mgmt_listen = INVALID_SOCKET;
    }
}

static void stop_mgmt_proxy(void)
{
    if (g_mgmt_thread)
    {
        InterlockedExchange(&g_mgmt_stop, 1);
        WaitForSingleObject(g_mgmt_thread, 3000);
        CloseHandle(g_mgmt_thread);
        g_mgmt_thread = NULL;
    }
    if (g_mgmt_listen != INVALID_SOCKET)
    {
        closesocket(g_mgmt_listen);
        g_mgmt_listen = INVALID_SOCKET;
    }
}

/*
 * 构建 Windows 命令行字符串（exe 路径 + 参数，含引号处理）。
 */
static char *build_command_line(const char *exe, char **args, int argc)
{
    size_t cap = strlen(exe) + 4;
    size_t len = 0;
    char *cmd = (char *)malloc(cap);
    int i;

    if (!cmd)
    {
        return NULL;
    }
    cmd[0] = '\0';

    /* exe 路径加引号 */
    if (len + strlen(exe) + 3 >= cap)
    {
        cap = len + strlen(exe) + 128;
        cmd = (char *)realloc(cmd, cap);
        if (!cmd)
        {
            return NULL;
        }
    }
    strcat(cmd, "\"");
    strcat(cmd, exe);
    strcat(cmd, "\"");
    len = strlen(cmd);

    for (i = 1; i < argc; i++)
    {
        const char *a = args[i];

        size_t need = len + strlen(a) + 4; /* 空格 + 引号 + 结尾 */
        if (need >= cap)
        {
            cap = need + 256;
            cmd = (char *)realloc(cmd, cap);
            if (!cmd)
            {
                return NULL;
            }
        }
        strcat(cmd, " ");
        /* 含空格或引号的参数加引号 */
        if (strchr(a, ' ') || strchr(a, '\t') || strchr(a, '"'))
        {
            strcat(cmd, "\"");
            strcat(cmd, a);
            strcat(cmd, "\"");
        }
        else
        {
            strcat(cmd, a);
        }
        len = strlen(cmd);
    }
    return cmd;
}

/*
 * Windows 启动：手动 CreateProcess，直接透传 service 传入的标准句柄。
 * 不 DuplicateHandle、不用 HANDLE_LIST：bInheritHandles=TRUE 全量继承可继承句柄，
 * 使 --msg-channel 的句柄值在子进程中保持不变，且 stdout/stderr 保持同一句柄。
 */
static int launch_openvpn_windows(const char *exe, int argc, char **argv)
{
    char *cmdline;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code;

    cmdline = build_command_line(exe, argv, argc);
    if (!cmdline)
    {
        shim_report(argc, argv, "构建命令行失败");
        return 1;
    }

    shim_log("exe=%s", exe);
    shim_log("cmdline=%s", cmdline);

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    shim_log("hIn=%p hOut=%p hErr=%p",
             si.hStdInput, si.hStdOutput, si.hStdError);

    {
        /* 转成宽字符命令行 */
        int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
        wchar_t *wcmd = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        BOOL ok;
        DWORD err;
        if (!wcmd)
        {
            free(cmdline);
            return 1;
        }
        MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wcmd, wlen);

        /* bInheritHandles=TRUE：继承所有可继承句柄，msg-channel 句柄值不变。
         * CREATE_NO_WINDOW：避免 openvpn-real.exe 分配控制台窗口（对齐 interactive service）。
         * 继承 shim 的进程优先级，与 service 用 settings.priority 启动的优先级一致。 */
        {
            DWORD flags = CREATE_NO_WINDOW;
            DWORD priority = GetPriorityClass(GetCurrentProcess());
            if (priority)
            {
                flags |= priority;
            }
            ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, flags, NULL, NULL, &si, &pi);
            err = ok ? 0 : GetLastError();
        }
        free(wcmd);
        shim_log("CreateProcess ok=%d err=%lu", ok, err);
        if (!ok)
        {
            shim_report(argc, argv, "启动 %s 失败: %lu", exe, err);
            free(cmdline);
            return 1;
        }
    }

    free(cmdline);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)exit_code;
}

#else /* POSIX */

/*
 * POSIX 启动：fork + execvp，透传 stdin/stdout/stderr。
 */
static int launch_openvpn_posix(const char *exe, int argc, char **argv)
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
    {
        shim_report(argc, argv, "fork 失败");
        return 1;
    }
    if (pid == 0)
    {
        /* 子进程：exec */
        char **child_argv = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
        int i;
        if (!child_argv)
        {
            _exit(127);
        }
        child_argv[0] = (char *)exe;
        for (i = 1; i < argc; i++)
        {
            child_argv[i] = argv[i];
        }
        child_argv[argc] = NULL;
        execvp(exe, child_argv);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0)
    {
        return 1;
    }
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    return 1;
}

#endif

/*
 * 程序入口。
 */
int main(int argc, char **argv)
{
    const char *real_openvpn;
    int code;
#ifdef _WIN32
    WSADATA wsa;
    int wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif

    srand((unsigned int)time(NULL));

    real_openvpn = find_real_openvpn(argv[0]);

    if (process_remote_srv(argc, argv) != 0)
    {
        return 1;
    }

#ifdef _WIN32
    if (wsa_ok)
    {
        setup_mgmt_proxy(argc, argv);
        start_mgmt_proxy();
    }
    code = launch_openvpn_windows(real_openvpn, argc, argv);
    if (wsa_ok)
    {
        stop_mgmt_proxy();
        WSACleanup();
    }
#else
    code = launch_openvpn_posix(real_openvpn, argc, argv);
#endif

    if (g_tmp_config)
    {
        remove(g_tmp_config);
    }
    return code;
}

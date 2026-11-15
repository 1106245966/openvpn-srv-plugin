/*
 * srv.c - 独立的 DNS SRV 查询实现（跨平台）
 *
 * POSIX（Linux/Android/macOS/BSD）：res_query + 手动解析 DNS 报文
 * Windows：DnsQuery_W
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef _WIN32
#define strdup _strdup
#endif

#include "srv.h"

/* DNS 记录类型常量（避免依赖平台差异的 nameser.h） */
#define DNS_TYPE_SRV_VALUE 33
#define DNS_CLASS_IN_VALUE 1

/* 单次查询最多解析的 SRV 记录条数 */
#define MAX_SRV_RECORDS 16

/* 解析出的一条 SRV 记录 */
struct srv_record {
    uint16_t priority;
    uint16_t weight;
    uint16_t port;
    char target[256];
};

/*
 * 写入失败原因，便于 shim 把具体原因记到日志里。
 */
static void srv_set_error(char *errbuf, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (!errbuf || errlen == 0)
    {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

/*
 * 清空错误文本（成功路径）。
 */
static void srv_clear_error(char *errbuf, size_t errlen)
{
    if (errbuf && errlen > 0)
    {
        errbuf[0] = '\0';
    }
}

/*
 * 从 `_service._proto.name` 中提取协议，返回 0(udp)/1(tcp)，无法识别返回 -1。
 */
static int srv_proto_from_name(const char *srv_name)
{
    const char *p1 = strchr(srv_name, '.');
    const char *p2;
    if (!p1)
    {
        return -1;
    }
    p2 = strchr(p1 + 1, '.');
    if (!p2)
    {
        return -1;
    }

    if ((size_t)(p2 - (p1 + 1)) == 4 && strncmp(p1 + 1, "_tcp", 4) == 0)
    {
        return 1;
    }
    if ((size_t)(p2 - (p1 + 1)) == 4 && strncmp(p1 + 1, "_udp", 4) == 0)
    {
        return 0;
    }
    return -1;
}

/*
 * 按 RFC 2782 的 priority/weight 规则，从候选记录中选出一条。
 * 返回选中记录的下标，无记录时返回 -1。
 */
static int select_srv_record(const struct srv_record *records, int count)
{
    uint16_t min_priority;
    int index[MAX_SRV_RECORDS];
    int n = 0;
    int total_weight = 0;
    int i;
    int pick, acc;

    if (count <= 0)
    {
        return -1;
    }

    /* 找出最低 priority */
    min_priority = records[0].priority;
    for (i = 1; i < count; ++i)
    {
        if (records[i].priority < min_priority)
        {
            min_priority = records[i].priority;
        }
    }

    /* 收集最低优先级组 */
    for (i = 0; i < count; ++i)
    {
        if (records[i].priority == min_priority)
        {
            index[n++] = i;
        }
    }

    /* 计算权重和，权重全零时退化为等概率 */
    for (i = 0; i < n; ++i)
    {
        total_weight += records[index[i]].weight;
    }
    if (total_weight == 0)
    {
        return index[rand() % n];
    }

    /* 加权随机选择 */
    pick = rand() % total_weight;
    acc = 0;
    for (i = 0; i < n; ++i)
    {
        acc += records[index[i]].weight;
        if (pick < acc)
        {
            return index[i];
        }
    }
    return index[n - 1];
}

#if defined(_WIN32)

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windns.h>

/*
 * Windows 实现：使用 DnsQuery_W 查询 SRV 记录。
 */
int srv_resolve(const char *srv_name, char **host_out, int *port_out, int *proto_out,
                char *errbuf, size_t errlen)
{
    int proto = srv_proto_from_name(srv_name);
    int wlen;
    wchar_t *wqname;
    PDNS_RECORD result = NULL;
    DNS_STATUS status;
    struct srv_record records[MAX_SRV_RECORDS];
    int count = 0;
    int sel;
    PDNS_RECORD r;

    srv_clear_error(errbuf, errlen);
    if (host_out)
    {
        *host_out = NULL;
    }
    if (proto < 0)
    {
        srv_set_error(errbuf, errlen, "名称中缺少 _tcp/_udp 协议段");
        return -1;
    }

    /* 将 UTF-8 查询名转为宽字符 */
    wlen = MultiByteToWideChar(CP_UTF8, 0, srv_name, -1, NULL, 0);
    if (wlen <= 0)
    {
        srv_set_error(errbuf, errlen, "查询名转为宽字符失败");
        return -1;
    }
    wqname = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wqname)
    {
        srv_set_error(errbuf, errlen, "内存不足");
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, srv_name, -1, wqname, wlen);

    status = DnsQuery_W(wqname, DNS_TYPE_SRV, DNS_QUERY_STANDARD, NULL, &result, NULL);
    free(wqname);
    if (status != 0 || !result)
    {
        srv_set_error(errbuf, errlen, "DnsQuery_W 返回 status=%ld", (long)status);
        return -1;
    }

    /* 收集 SRV 记录 */
    for (r = result; r; r = r->pNext)
    {
        int alen;
        if (r->wType != DNS_TYPE_SRV || count >= MAX_SRV_RECORDS)
        {
            continue;
        }
        records[count].priority = r->Data.SRV.wPriority;
        records[count].weight = r->Data.SRV.wWeight;
        records[count].port = r->Data.SRV.wPort;
        alen = WideCharToMultiByte(CP_UTF8, 0, r->Data.SRV.pNameTarget, -1,
                                   records[count].target, sizeof(records[count].target),
                                   NULL, NULL);
        if (alen <= 0)
        {
            continue;
        }
        count++;
    }
    DnsRecordListFree(result, DnsFreeRecordList);

    if (count == 0)
    {
        srv_set_error(errbuf, errlen, "应答中没有 SRV 记录");
        return -1;
    }
    sel = select_srv_record(records, count);
    if (sel < 0)
    {
        return -1;
    }

    if (host_out)
    {
        *host_out = strdup(records[sel].target);
        if (!*host_out)
        {
            srv_set_error(errbuf, errlen, "内存不足");
            return -1;
        }
    }
    if (port_out)
    {
        *port_out = (int)records[sel].port;
    }
    if (proto_out)
    {
        *proto_out = proto;
    }
    return 0;
}

#else /* POSIX（Linux/Android/macOS/BSD 均提供 resolv.h） */

#include <resolv.h>

/*
 * 读取 DNS 报文中的 16 位大端无符号整数。
 */
static uint16_t dns_read_u16(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/*
 * 跳过 DNS 报文中的域名（处理压缩指针），返回域名之后的指针，失败返回 NULL。
 */
static const unsigned char *dns_skip_name(const unsigned char *msg,
                                          const unsigned char *end,
                                          const unsigned char *p)
{
    (void)msg;

    while (p < end)
    {
        unsigned char c = *p;
        if (c == 0)
        {
            return p + 1;
        }
        if ((c & 0xC0) == 0xC0)
        {
            if (p + 1 >= end)
            {
                return NULL;
            }
            return p + 2;
        }
        p += 1 + c;
    }
    return NULL;
}

/*
 * 解析 DNS 报文中的压缩域名，写入 out 缓冲区，返回长度，失败返回 -1。
 */
static int dns_decode_name(const unsigned char *msg, const unsigned char *end,
                           const unsigned char *p, char *out, size_t outlen)
{
    size_t pos = 0;
    const unsigned char *cur = p;
    int jumps = 0;

    while (1)
    {
        unsigned char c;
        if (cur >= end)
        {
            return -1;
        }
        c = *cur;
        if (c == 0)
        {
            break;
        }
        if ((c & 0xC0) == 0xC0)
        {
            uint16_t offset;
            if (cur + 1 >= end)
            {
                return -1;
            }
            offset = (uint16_t)(((c & 0x3F) << 8) | cur[1]);
            if (msg + offset >= end)
            {
                return -1;
            }
            cur = msg + offset;
            if (++jumps > 32)
            {
                return -1;
            }
            continue;
        }
        if (cur + 1 + c > end)
        {
            return -1;
        }
        if (pos > 0)
        {
            if (pos + 1 >= outlen)
            {
                return -1;
            }
            out[pos++] = '.';
        }
        if (pos + c >= outlen)
        {
            return -1;
        }
        memcpy(out + pos, cur + 1, c);
        pos += c;
        cur += 1 + c;
    }
    out[pos] = '\0';
    return (int)pos;
}

/*
 * 从 res_query 返回的原始 DNS 报文中解析 SRV 记录，返回解析条数。
 */
static int parse_srv_response(const unsigned char *msg, int len,
                              struct srv_record *records, int max_records)
{
    uint16_t qdcount, ancount;
    const unsigned char *p;
    const unsigned char *end;
    int count = 0;
    int i;

    if (len < 12)
    {
        return 0;
    }
    qdcount = dns_read_u16(msg + 4);
    ancount = dns_read_u16(msg + 6);

    p = msg + 12;
    end = msg + len;

    /* 跳过 question section */
    for (i = 0; i < qdcount; ++i)
    {
        p = dns_skip_name(msg, end, p);
        if (!p || p + 4 > end)
        {
            return 0;
        }
        p += 4;
    }

    /* 遍历 answer section，提取 SRV 记录 */
    for (i = 0; i < ancount; ++i)
    {
        uint16_t type, rdlength;
        const unsigned char *rdata;
        int nlen;

        p = dns_skip_name(msg, end, p);
        if (!p || p + 10 > end)
        {
            return count;
        }
        type = dns_read_u16(p);
        rdlength = dns_read_u16(p + 8);
        rdata = p + 10;
        if (rdata + rdlength > end)
        {
            return count;
        }
        p = rdata + rdlength;

        if (type == DNS_TYPE_SRV_VALUE && rdlength >= 7 && count < max_records)
        {
            records[count].priority = dns_read_u16(rdata);
            records[count].weight = dns_read_u16(rdata + 2);
            records[count].port = dns_read_u16(rdata + 4);
            nlen = dns_decode_name(msg, end, rdata + 6,
                                   records[count].target, sizeof(records[count].target));
            if (nlen <= 0)
            {
                continue;
            }
            count++;
        }
    }
    return count;
}

/*
 * POSIX 实现：使用 res_query 查询 SRV 记录并手动解析响应。
 */
int srv_resolve(const char *srv_name, char **host_out, int *port_out, int *proto_out,
                char *errbuf, size_t errlen)
{
    int proto = srv_proto_from_name(srv_name);
    unsigned char answer[2048];
    int len;
    struct srv_record records[MAX_SRV_RECORDS];
    int count;
    int sel;

    srv_clear_error(errbuf, errlen);
    if (host_out)
    {
        *host_out = NULL;
    }
    if (proto < 0)
    {
        srv_set_error(errbuf, errlen, "名称中缺少 _tcp/_udp 协议段");
        return -1;
    }

    len = res_query(srv_name, DNS_CLASS_IN_VALUE, DNS_TYPE_SRV_VALUE, answer, sizeof(answer));
    if (len < 0)
    {
        srv_set_error(errbuf, errlen, "res_query 失败（名称不存在或无可用 DNS）");
        return -1;
    }

    count = parse_srv_response(answer, len, records, MAX_SRV_RECORDS);
    if (count == 0)
    {
        srv_set_error(errbuf, errlen, "应答中没有 SRV 记录");
        return -1;
    }
    sel = select_srv_record(records, count);
    if (sel < 0)
    {
        return -1;
    }

    if (host_out)
    {
        *host_out = strdup(records[sel].target);
        if (!*host_out)
        {
            srv_set_error(errbuf, errlen, "内存不足");
            return -1;
        }
    }
    if (port_out)
    {
        *port_out = (int)records[sel].port;
    }
    if (proto_out)
    {
        *proto_out = proto;
    }
    return 0;
}

#endif /* _WIN32 */

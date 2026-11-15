/*
 * srv.h - 独立的 DNS SRV 查询模块（跨平台）
 *
 * 供 C shim 使用，不依赖 OpenVPN 源码。POSIX 走 res_query + 手动报文解析，
 * Windows 走 DnsQuery_W。
 */

#ifndef SRV_H
#define SRV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 解析 DNS SRV 记录，按 RFC 2782 的 priority/weight 规则选出一个目标服务器。
 *
 * @param srv_name   完整的 SRV 查询名，形如 _service._proto.name
 * @param host_out   输出目标主机名（内部 malloc，调用者需 free），失败时为 NULL
 * @param port_out   输出目标端口
 * @param proto_out  输出协议：0 = udp，1 = tcp
 * @param errbuf     失败原因文本（可为 NULL）；成功时写为空串
 * @param errlen     errbuf 的字节数
 * @return           0 成功，-1 失败
 */
int srv_resolve(const char *srv_name, char **host_out, int *port_out, int *proto_out,
                char *errbuf, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* SRV_H */

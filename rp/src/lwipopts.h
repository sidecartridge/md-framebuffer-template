#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// Common settings used in most of the pico_w examples
// (see https://www.nongnu.org/lwip/2_1_x/group__lwip__opts.html for details)

// allow override in some examples
#ifndef NO_SYS
#define NO_SYS 1
#endif

// allow override in some examples
#ifndef LWIP_SOCKET
#define LWIP_SOCKET 0
#endif
#if PICO_CYW43_ARCH_POLL
#define MEM_LIBC_MALLOC 1
#else
// MEM_LIBC_MALLOC is incompatible with non polling versions
#define MEM_LIBC_MALLOC 0
#endif

#define MEM_ALIGNMENT 4
#define MEM_SIZE 4096

#define MEM_SANITY_CHECK 0
#define MEM_OVERFLOW_CHECK 0

#define MEMP_NUM_PBUF 8
#define MEMP_NUM_TCP_PCB 4
#define MEMP_NUM_TCP_SEG 16
#define MEMP_NUM_ARP_QUEUE 2
#define PBUF_POOL_SIZE 12
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define TCP_MSS 1460
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN ((2 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETCONN 0
#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0
// #define ETH_PAD_SIZE                2
#define LWIP_CHKSUM_ALGORITHM 3
#define LWIP_DHCP 1
#define LWIP_IPV4 1
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_TCP_KEEPALIVE 0
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_DHCP_GET_NTP_SRV 0

#ifndef NDEBUG
#define LWIP_DEBUG 1
#define LWIP_STATS 1
#define LWIP_STATS_DISPLAY 1
#endif

#define ETHARP_DEBUG LWIP_DBG_OFF
#define NETIF_DEBUG LWIP_DBG_OFF
#define PBUF_DEBUG LWIP_DBG_OFF
#define API_LIB_DEBUG LWIP_DBG_OFF
#define API_MSG_DEBUG LWIP_DBG_OFF
#define SOCKETS_DEBUG LWIP_DBG_OFF
#define ICMP_DEBUG LWIP_DBG_OFF
#define INET_DEBUG LWIP_DBG_OFF
#define IP_DEBUG LWIP_DBG_OFF
#define IP_REASS_DEBUG LWIP_DBG_OFF
#define RAW_DEBUG LWIP_DBG_OFF
#define MEM_DEBUG LWIP_DBG_OFF
#define MEMP_DEBUG LWIP_DBG_OFF
#define SYS_DEBUG LWIP_DBG_OFF
#define TCP_DEBUG LWIP_DBG_OFF
#define TCP_INPUT_DEBUG LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG LWIP_DBG_OFF
#define TCP_RTO_DEBUG LWIP_DBG_OFF
#define TCP_CWND_DEBUG LWIP_DBG_OFF
#define TCP_WND_DEBUG LWIP_DBG_OFF
#define TCP_FR_DEBUG LWIP_DBG_OFF
#define TCP_QLEN_DEBUG LWIP_DBG_OFF
#define TCP_RST_DEBUG LWIP_DBG_OFF
#define UDP_DEBUG LWIP_DBG_OFF
#define TCPIP_DEBUG LWIP_DBG_OFF
#define PPP_DEBUG LWIP_DBG_OFF
#define SLIP_DEBUG LWIP_DBG_OFF
#define DHCP_DEBUG LWIP_DBG_OFF

// Custom flags
// #define TCP_FAST_INTERVAL 50
#define TCP_NODELAY 1

#define LWIP_NETIF_API \
  0  //  Not needed. Sequential API, and therefore for platforms with OSes only.
#define LWIP_SOCKET \
  0  //  Not needed. Sequential API, and therefore for platforms with OSes only.

#define LWIP_HTTPD 0

#if LWIP_HTTPD == 1
#define LWIP_HTTPD_SSI 1
#define LWIP_HTTPD_CGI 1
// don't include the tag comment - less work for the CPU, but may be harder to
// debug
#define LWIP_HTTPD_SSI_INCLUDE_TAG 0
#define LWIP_HTTPD_SSI_MULTIPART 1
#define LWIP_HTTPD_DYNAMIC_HEADERS 0
#define LWIP_HTTPD_SUPPORT_POST 1
#define LWIP_HTTPD_SUPPORT_11_KEEPALIVE 1

#define LWIP_HTTPD_FS_ASYNC_READ 1
#define HTTPD_POLL_INTERVAL 1
#define HTTPD_PRECALCULATED_CHECKSUM 1
#define HTTPD_USE_MEM_POOL 1
#define MEMP_NUM_PARALLEL_HTTPD_CONNS 4
#define MEMP_NUM_PARALLEL_HTTPD_SSI_CONNS 4
#define LWIP_HTTPD_ABORT_ON_CLOSE_MEM_ERROR 1

#define HTTPD_FSDATA_FILE "fsdata_srv.c"
#endif

// Only plain HTTP client: keep ALTCP/TLS disabled to save memory.
#define LWIP_ALTCP 0
#define MEMP_NUM_ALTCP_PCB 0
#define LWIP_ALTCP_TLS 0
#define LWIP_ALTCP_TLS_MBEDTLS 0

// Note bug in lwip with LWIP_ALTCP and LWIP_DEBUG
// https://savannah.nongnu.org/bugs/index.php?62159
// #define LWIP_DEBUG 1
// #undef LWIP_DEBUG
// #define LWIP_DEBUG                  1
// #define MEMP_OVERFLOW_CHECK         2
// #define MEMP_SANITY_CHECK           1

#endif /* __LWIPOPTS_H__ */

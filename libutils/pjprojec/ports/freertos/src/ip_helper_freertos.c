/*
 * ip_helper_freertos.c
 *
 * PJLIB IP helper for FreeRTOS + lwIP. Enumerates interface addresses from
 * lwIP's netif list (the default netif is the LAN9118 Ethernet with
 * 10.0.2.15 under QEMU slirp).
 */
#include <pj/ip_helper.h>
#include <pj/sock.h>
#include <pj/errno.h>
#include <pj/assert.h>
#include <pj/log.h>
#include <pj/string.h>

#include <lwip/netif.h>
#include <lwip/ip_addr.h>

#define THIS_FILE   "ip_helper_freertos.c"

PJ_DEF(pj_status_t) pj_enum_ip_interface(int af, unsigned *count,
                                         pj_sockaddr ifs[])
{
    struct netif *nif;
    unsigned max, n = 0;

    PJ_ASSERT_RETURN(count && ifs, PJ_EINVAL);

    if (af != PJ_AF_INET)
        return PJ_EAFNOTSUP;

    max = *count;

    for (nif = netif_list; nif && n < max; nif = nif->next) {
        if (!netif_is_up(nif))
            continue;

#if LWIP_IPV4
        pj_sockaddr_in_init(&ifs[n].ipv4, NULL, 0);
        ifs[n].ipv4.sin_addr.s_addr = ip4_addr_get_u32(netif_ip4_addr(nif));
        ++n;
#endif
    }

    *count = n;
    return (n > 0) ? PJ_SUCCESS : PJ_ENOTFOUND;
}

PJ_DEF(pj_status_t) pj_enum_ip_interface2(const pj_enum_ip_option *opt,
                                          unsigned *count,
                                          pj_sockaddr ifs[])
{
    (void)opt;
    return pj_enum_ip_interface(PJ_AF_INET, count, ifs);
}

PJ_DEF(pj_status_t) pj_enum_ip_route(unsigned *count,
                                     pj_ip_route_entry routes[])
{
    (void)routes;
    if (count)
        *count = 0;
    return PJ_ENOTSUP;
}

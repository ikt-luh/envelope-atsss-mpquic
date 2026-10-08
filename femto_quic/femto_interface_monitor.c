/*
 * Femto-QUIC Interface Monitor
 * Monitors network interface state changes using Netlink sockets (Linux)
 * Enables dynamic path addition/removal for MP-QUIC
 * 
 * Author: Abebe S. Feleke
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#define USE_NETLINK 1
#else
#define USE_NETLINK 0
#endif

#include "femto_interface_monitor.h"
#include "femto_types.h"

#define NETLINK_BUFFER_SIZE 8192

/* Check if a network interface exists and is UP */
int femto_check_interface_available(int if_index)
{
    char ifname[IF_NAMESIZE];
    struct ifaddrs *ifaddr, *ifa;
    int is_up = 0;

    if (if_indextoname(if_index, ifname) == NULL) {
        return 0;  // Interface doesn't exist
    }

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return 0;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == NULL) continue;
        
        if (strcmp(ifa->ifa_name, ifname) == 0) {
            if (ifa->ifa_flags & IFF_UP) {
                is_up = 1;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    return is_up;
}

/* Get the IP address for a specific interface
 * Prefers IPv4 over IPv6. Rejects IPv6 link-local addresses unless IPv4 is not available.
 * For MP-QUIC, we need routable addresses, not link-local.
 */
int femto_get_interface_ip(int if_index, struct sockaddr_storage *ip)
{
    char ifname[IF_NAMESIZE];
    struct ifaddrs *ifaddr, *ifa;
    int found_ipv4 = 0;
    int found_ipv6_global = 0;
    int found_ipv6_linklocal = 0;
    struct sockaddr_storage ipv6_global_addr;
    struct sockaddr_storage ipv6_linklocal_addr;

    if (if_indextoname(if_index, ifname) == NULL) {
        return -1;  // Interface doesn't exist
    }

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == NULL || ifa->ifa_addr == NULL) continue;
        
        if (strcmp(ifa->ifa_name, ifname) == 0) {
            // IPv4 - always preferred
            if (ifa->ifa_addr->sa_family == AF_INET) {
                memcpy(ip, ifa->ifa_addr, sizeof(struct sockaddr_in));
                found_ipv4 = 1;
                break;  // IPv4 found, stop searching
            } 
            // IPv6 - categorize by type
            else if (ifa->ifa_addr->sa_family == AF_INET6) {
                struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)ifa->ifa_addr;
                
                // Check if link-local (fe80::/10)
                if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr)) {
                    if (!found_ipv6_linklocal) {
                        memcpy(&ipv6_linklocal_addr, ifa->ifa_addr, sizeof(struct sockaddr_in6));
                        found_ipv6_linklocal = 1;
                    }
                } else {
                    // Global IPv6 address
                    if (!found_ipv6_global) {
                        memcpy(&ipv6_global_addr, ifa->ifa_addr, sizeof(struct sockaddr_in6));
                        found_ipv6_global = 1;
                    }
                }
            }
        }
    }

    freeifaddrs(ifaddr);
    
    // Priority: IPv4 > IPv6 Global > IPv6 Link-local
    if (found_ipv4) {
        return 0;  // Already copied to ip
    } else if (found_ipv6_global) {
        memcpy(ip, &ipv6_global_addr, sizeof(struct sockaddr_in6));
        return 0;
    } else if (found_ipv6_linklocal) {
        // Return link-local but caller should check and potentially wait for better address
        memcpy(ip, &ipv6_linklocal_addr, sizeof(struct sockaddr_in6));
        return 0;
    }
    
    return -1;  // No address found
}

/* Get the IP address for a specific interface - IPv4 only version
 * Returns -1 if no IPv4 address is available (caller should wait or use IPv6)
 */
int femto_get_interface_ipv4(int if_index, struct sockaddr_storage *ip)
{
    char ifname[IF_NAMESIZE];
    struct ifaddrs *ifaddr, *ifa;
    int found = 0;

    if (if_indextoname(if_index, ifname) == NULL) {
        return -1;  // Interface doesn't exist
    }

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == NULL || ifa->ifa_addr == NULL) continue;
        
        if (strcmp(ifa->ifa_name, ifname) == 0) {
            if (ifa->ifa_addr->sa_family == AF_INET) {
                memcpy(ip, ifa->ifa_addr, sizeof(struct sockaddr_in));
                found = 1;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    return found ? 0 : -1;
}

/* Get interface name from index */
int femto_get_interface_name(int if_index, char *ifname, size_t len)
{
    if (if_indextoname(if_index, ifname) == NULL) {
        return -1;
    }
    return 0;
}

/* Get interface index from name */
int femto_get_interface_index(const char *ifname)
{
    unsigned int idx = if_nametoindex(ifname);
    return (idx == 0) ? -1 : (int)idx;
}

/* Initialize Netlink socket for interface monitoring (Linux only) */
int femto_init_netlink_socket(void)
{
#if USE_NETLINK
    int sock;
    struct sockaddr_nl addr;

    sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0) {
        perror("socket(NETLINK_ROUTE)");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    // Listen for both link state changes AND address changes
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(netlink)");
        close(sock);
        return -1;
    }

    return sock;
#else
    // On non-Linux platforms, return a dummy socket (polling mode)
    printf("[Interface Monitor] Netlink not available, using polling mode\n");
    return -2;  // Special value indicating polling mode
#endif
}

/* Helper: Compare two sockaddr_storage addresses */
static int sockaddr_equal(struct sockaddr_storage *a, struct sockaddr_storage *b)
{
    if (a->ss_family != b->ss_family) return 0;
    
    if (a->ss_family == AF_INET) {
        struct sockaddr_in *a4 = (struct sockaddr_in *)a;
        struct sockaddr_in *b4 = (struct sockaddr_in *)b;
        return a4->sin_addr.s_addr == b4->sin_addr.s_addr;
    } else if (a->ss_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)a;
        struct sockaddr_in6 *b6 = (struct sockaddr_in6 *)b;
        return memcmp(&a6->sin6_addr, &b6->sin6_addr, sizeof(struct in6_addr)) == 0;
    }
    return 0;
}

/* Helper: Convert sockaddr_storage to string */
static void sockaddr_to_string(struct sockaddr_storage *addr, char *buf, size_t len)
{
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in *a4 = (struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &a4->sin_addr, buf, len);
    } else if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &a6->sin6_addr, buf, len);
    } else {
        snprintf(buf, len, "unknown");
    }
}

/* Helper: Update tracked IP for an interface */
static void update_tracked_ip(femto_interface_monitor_ctx_t *ctx, int if_index, 
                               struct sockaddr_storage *new_ip, struct sockaddr_storage *old_ip_out)
{
    // Find existing entry
    for (int i = 0; i < ctx->tracked_ip_count; i++) {
        if (ctx->tracked_ips[i].if_index == if_index && ctx->tracked_ips[i].valid) {
            if (old_ip_out) {
                memcpy(old_ip_out, &ctx->tracked_ips[i].ip, sizeof(struct sockaddr_storage));
            }
            memcpy(&ctx->tracked_ips[i].ip, new_ip, sizeof(struct sockaddr_storage));
            return;
        }
    }
    
    // Add new entry
    if (ctx->tracked_ip_count < 16) {
        ctx->tracked_ips[ctx->tracked_ip_count].if_index = if_index;
        memcpy(&ctx->tracked_ips[ctx->tracked_ip_count].ip, new_ip, sizeof(struct sockaddr_storage));
        ctx->tracked_ips[ctx->tracked_ip_count].valid = 1;
        ctx->tracked_ip_count++;
    }
    
    if (old_ip_out) {
        memset(old_ip_out, 0, sizeof(struct sockaddr_storage));
    }
}

/* Helper: Get tracked IP for an interface */
static int get_tracked_ip(femto_interface_monitor_ctx_t *ctx, int if_index, 
                          struct sockaddr_storage *ip_out)
{
    for (int i = 0; i < ctx->tracked_ip_count; i++) {
        if (ctx->tracked_ips[i].if_index == if_index && ctx->tracked_ips[i].valid) {
            if (ip_out) {
                memcpy(ip_out, &ctx->tracked_ips[i].ip, sizeof(struct sockaddr_storage));
            }
            return 1;
        }
    }
    return 0;
}

/* Helper: Remove tracked IP for an interface */
static void remove_tracked_ip(femto_interface_monitor_ctx_t *ctx, int if_index)
{
    for (int i = 0; i < ctx->tracked_ip_count; i++) {
        if (ctx->tracked_ips[i].if_index == if_index) {
            ctx->tracked_ips[i].valid = 0;
            return;
        }
    }
}

#if USE_NETLINK
/* Message types for our handler */
#define MSG_TYPE_LINK_UP      1
#define MSG_TYPE_LINK_DOWN    2
#define MSG_TYPE_LINK_DEL     3
#define MSG_TYPE_ADDR_NEW     4
#define MSG_TYPE_ADDR_DEL     5

/* Parse a single Netlink message and extract interface info */
static int parse_netlink_message(struct nlmsghdr *nlh, int *if_index, int *is_up,
                                  struct sockaddr_storage *addr_out)
{
    if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK) {
        struct ifinfomsg *ifi = NLMSG_DATA(nlh);
        *if_index = ifi->ifi_index;
        *is_up = (ifi->ifi_flags & IFF_UP) && (ifi->ifi_flags & IFF_RUNNING);
        
        if (nlh->nlmsg_type == RTM_DELLINK) {
            return MSG_TYPE_LINK_DEL;
        }
        return *is_up ? MSG_TYPE_LINK_UP : MSG_TYPE_LINK_DOWN;
    }
    
    if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR) {
        struct ifaddrmsg *ifa = NLMSG_DATA(nlh);
        *if_index = ifa->ifa_index;
        
        // Parse address from attributes
        struct rtattr *rta = IFA_RTA(ifa);
        int rtl = IFA_PAYLOAD(nlh);
        
        while (RTA_OK(rta, rtl)) {
            if (rta->rta_type == IFA_LOCAL || rta->rta_type == IFA_ADDRESS) {
                if (ifa->ifa_family == AF_INET) {
                    struct sockaddr_in *sin = (struct sockaddr_in *)addr_out;
                    memset(sin, 0, sizeof(*sin));
                    sin->sin_family = AF_INET;
                    memcpy(&sin->sin_addr, RTA_DATA(rta), sizeof(struct in_addr));
                } else if (ifa->ifa_family == AF_INET6) {
                    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr_out;
                    memset(sin6, 0, sizeof(*sin6));
                    sin6->sin6_family = AF_INET6;
                    memcpy(&sin6->sin6_addr, RTA_DATA(rta), sizeof(struct in6_addr));
                }
                break;
            }
            rta = RTA_NEXT(rta, rtl);
        }
        
        return (nlh->nlmsg_type == RTM_NEWADDR) ? MSG_TYPE_ADDR_NEW : MSG_TYPE_ADDR_DEL;
    }
    
    return -1;  // Unknown message type
}
#endif

/* Interface monitor thread function */
void *femto_monitor_interface_events(void *arg)
{
    femto_interface_monitor_ctx_t *monitor_ctx = (femto_interface_monitor_ctx_t *)arg;
    
    printf("[Interface Monitor] Started monitoring for interface and address changes\n");

#if USE_NETLINK
    char buffer[NETLINK_BUFFER_SIZE];
    struct nlmsghdr *nlh;
    int len;
    int if_index, is_up;
    int msg_type;
    struct sockaddr_storage new_addr;

    while (monitor_ctx->running) {
        len = recv(monitor_ctx->netlink_socket, buffer, sizeof(buffer), 0);
        if (len < 0) {
            if (errno == EINTR) continue;
            perror("recv(netlink)");
            break;
        }

        for (nlh = (struct nlmsghdr *)buffer; NLMSG_OK(nlh, (unsigned int)len); nlh = NLMSG_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE) break;
            if (nlh->nlmsg_type == NLMSG_ERROR) {
                fprintf(stderr, "[Interface Monitor] Netlink error\n");
                continue;
            }

            memset(&new_addr, 0, sizeof(new_addr));
            msg_type = parse_netlink_message(nlh, &if_index, &is_up, &new_addr);
            if (msg_type < 0) continue;

            char ifname[IF_NAMESIZE];
            if (if_indextoname(if_index, ifname) == NULL) {
                snprintf(ifname, sizeof(ifname), "if%d", if_index);
            }

            switch (msg_type) {
                case MSG_TYPE_LINK_UP:
                    printf("[Interface Monitor] Interface %s (index=%d) is UP\n", ifname, if_index);
                    if (monitor_ctx->on_interface_up) {
                        monitor_ctx->on_interface_up(monitor_ctx->user_ctx, if_index);
                    }
                    break;
                    
                case MSG_TYPE_LINK_DOWN:
                    printf("[Interface Monitor] Interface %s (index=%d) is DOWN\n", ifname, if_index);
                    remove_tracked_ip(monitor_ctx, if_index);
                    if (monitor_ctx->on_interface_down) {
                        monitor_ctx->on_interface_down(monitor_ctx->user_ctx, if_index);
                    }
                    break;
                    
                case MSG_TYPE_LINK_DEL:
                    printf("[Interface Monitor] Interface %s (index=%d) DELETED\n", ifname, if_index);
                    remove_tracked_ip(monitor_ctx, if_index);
                    if (monitor_ctx->on_interface_down) {
                        monitor_ctx->on_interface_down(monitor_ctx->user_ctx, if_index);
                    }
                    break;
                    
                case MSG_TYPE_ADDR_NEW: {
                    char new_ip_str[INET6_ADDRSTRLEN];
                    sockaddr_to_string(&new_addr, new_ip_str, sizeof(new_ip_str));
                    
                    // Check if this is an IP change (we already had an IP for this interface)
                    struct sockaddr_storage old_addr;
                    int had_old_ip = get_tracked_ip(monitor_ctx, if_index, &old_addr);
                    
                    if (had_old_ip && !sockaddr_equal(&old_addr, &new_addr)) {
                        // IP address changed!
                        char old_ip_str[INET6_ADDRSTRLEN];
                        sockaddr_to_string(&old_addr, old_ip_str, sizeof(old_ip_str));
                        
                        printf("[Interface Monitor] Interface %s (index=%d) IP CHANGED: %s -> %s\n", 
                               ifname, if_index, old_ip_str, new_ip_str);
                        
                        if (monitor_ctx->on_addr_change) {
                            femto_addr_change_info_t change_info;
                            change_info.if_index = if_index;
                            memcpy(&change_info.old_ip, &old_addr, sizeof(old_addr));
                            memcpy(&change_info.new_ip, &new_addr, sizeof(new_addr));
                            change_info.has_old_ip = 1;
                            change_info.has_new_ip = 1;
                            monitor_ctx->on_addr_change(monitor_ctx->user_ctx, &change_info);
                        }
                    } else if (!had_old_ip) {
                        printf("[Interface Monitor] Interface %s (index=%d) got IP: %s\n", 
                               ifname, if_index, new_ip_str);
                    }
                    
                    // Update tracked IP
                    update_tracked_ip(monitor_ctx, if_index, &new_addr, NULL);
                    break;
                }
                    
                case MSG_TYPE_ADDR_DEL: {
                    char del_ip_str[INET6_ADDRSTRLEN];
                    sockaddr_to_string(&new_addr, del_ip_str, sizeof(del_ip_str));
                    printf("[Interface Monitor] Interface %s (index=%d) lost IP: %s\n", 
                           ifname, if_index, del_ip_str);

                    /* notify the user callback of address deletion.
                     * this is important for dynamic interface handling: when IPv4 is removed (e.g., link flap),
                     * the application should stop using that path immediately and steer traffic to other paths. */
                    if (monitor_ctx->on_addr_change) {
                        femto_addr_change_info_t change_info;
                        memset(&change_info, 0, sizeof(change_info));
                        change_info.if_index = if_index;
                        memcpy(&change_info.old_ip, &new_addr, sizeof(new_addr));
                        change_info.has_old_ip = 1;
                        change_info.has_new_ip = 0;
                        monitor_ctx->on_addr_change(monitor_ctx->user_ctx, &change_info);
                    }
                    
                    // Don't remove from tracking yet - wait for new IP or interface down
                    break;
                }
            }
        }
    }
#else
    // Polling mode for non-Linux platforms
    int prev_if_indices[32] = {0};
    int prev_count = 0;
    
    while (monitor_ctx->running) {
        int curr_if_indices[32];
        int curr_count = femto_list_interfaces(curr_if_indices, 32);
        
        // Check for new interfaces
        for (int i = 0; i < curr_count; i++) {
            int found = 0;
            for (int j = 0; j < prev_count; j++) {
                if (curr_if_indices[i] == prev_if_indices[j]) {
                    found = 1;
                    break;
                }
            }
            if (!found && monitor_ctx->on_interface_up) {
                char ifname[IF_NAMESIZE];
                if_indextoname(curr_if_indices[i], ifname);
                printf("[Interface Monitor] Interface %s (index=%d) is UP\n", ifname, curr_if_indices[i]);
                monitor_ctx->on_interface_up(monitor_ctx->user_ctx, curr_if_indices[i]);
            }
        }
        
        // Check for removed interfaces
        for (int i = 0; i < prev_count; i++) {
            int found = 0;
            for (int j = 0; j < curr_count; j++) {
                if (prev_if_indices[i] == curr_if_indices[j]) {
                    found = 1;
                    break;
                }
            }
            if (!found && monitor_ctx->on_interface_down) {
                printf("[Interface Monitor] Interface index=%d is DOWN\n", prev_if_indices[i]);
                remove_tracked_ip(monitor_ctx, prev_if_indices[i]);
                monitor_ctx->on_interface_down(monitor_ctx->user_ctx, prev_if_indices[i]);
            }
        }
        
        // Check for IP address changes on existing interfaces
        for (int i = 0; i < curr_count; i++) {
            struct sockaddr_storage curr_ip;
            if (femto_get_interface_ip(curr_if_indices[i], &curr_ip) == 0) {
                struct sockaddr_storage old_ip;
                int had_old = get_tracked_ip(monitor_ctx, curr_if_indices[i], &old_ip);
                
                if (had_old && !sockaddr_equal(&old_ip, &curr_ip)) {
                    // IP changed!
                    char ifname[IF_NAMESIZE];
                    char old_ip_str[INET6_ADDRSTRLEN], new_ip_str[INET6_ADDRSTRLEN];
                    if_indextoname(curr_if_indices[i], ifname);
                    sockaddr_to_string(&old_ip, old_ip_str, sizeof(old_ip_str));
                    sockaddr_to_string(&curr_ip, new_ip_str, sizeof(new_ip_str));
                    
                    printf("[Interface Monitor] Interface %s (index=%d) IP CHANGED: %s -> %s\n",
                           ifname, curr_if_indices[i], old_ip_str, new_ip_str);
                    
                    if (monitor_ctx->on_addr_change) {
                        femto_addr_change_info_t change_info;
                        change_info.if_index = curr_if_indices[i];
                        memcpy(&change_info.old_ip, &old_ip, sizeof(old_ip));
                        memcpy(&change_info.new_ip, &curr_ip, sizeof(curr_ip));
                        change_info.has_old_ip = 1;
                        change_info.has_new_ip = 1;
                        monitor_ctx->on_addr_change(monitor_ctx->user_ctx, &change_info);
                    }
                }
                
                update_tracked_ip(monitor_ctx, curr_if_indices[i], &curr_ip, NULL);
            }
        }
        
        // Update previous state
        memcpy(prev_if_indices, curr_if_indices, sizeof(curr_if_indices));
        prev_count = curr_count;
        
        sleep(2);  // Poll every 2 seconds
    }
#endif

    printf("[Interface Monitor] Stopped monitoring\n");
    return NULL;
}

/* Initialize the interface monitor */
int femto_init_interface_monitor(femto_interface_monitor_ctx_t *ctx,
                                  void *user_ctx,
                                  void (*on_up)(void *, int),
                                  void (*on_down)(void *, int))
{
    return femto_init_interface_monitor_full(ctx, user_ctx, on_up, on_down, NULL);
}

/* Initialize the interface monitor with address change callback */
int femto_init_interface_monitor_full(femto_interface_monitor_ctx_t *ctx,
                                       void *user_ctx,
                                       void (*on_up)(void *, int),
                                       void (*on_down)(void *, int),
                                       void (*on_addr_change)(void *, femto_addr_change_info_t *))
{
    if (ctx == NULL) return -1;

    memset(ctx, 0, sizeof(femto_interface_monitor_ctx_t));
    
    ctx->netlink_socket = femto_init_netlink_socket();
    if (ctx->netlink_socket < 0 && ctx->netlink_socket != -2) {
        return -1;
    }

    ctx->user_ctx = user_ctx;
    ctx->on_interface_up = on_up;
    ctx->on_interface_down = on_down;
    ctx->on_addr_change = on_addr_change;
    ctx->running = 0;
    ctx->tracked_ip_count = 0;

    return 0;
}

/* Start the interface monitor thread */
int femto_start_interface_monitor(femto_interface_monitor_ctx_t *ctx)
{
    if (ctx == NULL || ctx->running) return -1;

    ctx->running = 1;
    
    if (pthread_create(&ctx->monitor_thread, NULL, femto_monitor_interface_events, ctx) != 0) {
        perror("pthread_create(interface_monitor)");
        ctx->running = 0;
        return -1;
    }

    return 0;
}

/* Stop the interface monitor thread */
int femto_stop_interface_monitor(femto_interface_monitor_ctx_t *ctx)
{
    if (ctx == NULL || !ctx->running) return -1;

    ctx->running = 0;
    
    // Close socket to unblock recv()
    if (ctx->netlink_socket >= 0) {
        close(ctx->netlink_socket);
        ctx->netlink_socket = -1;
    }

    pthread_join(ctx->monitor_thread, NULL);
    
    return 0;
}

/* Cleanup the interface monitor */
void femto_cleanup_interface_monitor(femto_interface_monitor_ctx_t *ctx)
{
    if (ctx == NULL) return;

    if (ctx->running) {
        femto_stop_interface_monitor(ctx);
    }

    if (ctx->netlink_socket >= 0) {
        close(ctx->netlink_socket);
        ctx->netlink_socket = -1;
    }
}

/* List all available network interfaces */
int femto_list_interfaces(int *if_indices, int max_count)
{
    struct ifaddrs *ifaddr, *ifa;
    int count = 0;
    int seen[256] = {0};  // Track seen interface indices

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL && count < max_count; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == NULL) continue;
        
        unsigned int idx = if_nametoindex(ifa->ifa_name);
        if (idx == 0 || idx >= 256 || seen[idx]) continue;
        
        // Only include UP interfaces
        if (ifa->ifa_flags & IFF_UP) {
            if_indices[count++] = (int)idx;
            seen[idx] = 1;
        }
    }

    freeifaddrs(ifaddr);
    return count;
}


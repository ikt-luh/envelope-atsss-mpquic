/*
 * Femto-QUIC Interface Monitor Header
 * Monitors network interface state changes using Netlink sockets
 * 
 * Author: Abebe S. Feleke
 */

 #ifndef FEMTO_INTERFACE_MONITOR_H
 #define FEMTO_INTERFACE_MONITOR_H
 
 #include <pthread.h>
 #include <sys/socket.h>
 
 /* Structure to hold IP address change information */
 typedef struct st_femto_addr_change_info_t {
     int if_index;                                // Interface index
     struct sockaddr_storage old_ip;              // Previous IP address
     struct sockaddr_storage new_ip;              // New IP address
     int has_old_ip;                              // 1 if old_ip is valid
     int has_new_ip;                              // 1 if new_ip is valid
 } femto_addr_change_info_t;
 
 /* Interface monitor context structure */
 typedef struct st_femto_interface_monitor_ctx_t {
     int netlink_socket;                          // Netlink socket for receiving events
     pthread_t monitor_thread;                    // Monitor thread handle
     int running;                                 // Flag to control the monitor thread
     void *user_ctx;                              // User context passed to callbacks
     void (*on_interface_up)(void *, int);        // Callback when interface comes up
     void (*on_interface_down)(void *, int);      // Callback when interface goes down
     void (*on_addr_change)(void *, femto_addr_change_info_t *);  // Callback when IP address changes
     
     // Tracking current IP addresses for change detection
     struct {
         int if_index;
         struct sockaddr_storage ip;
         int valid;
     } tracked_ips[16];                           // Track up to 16 interfaces
     int tracked_ip_count;
 } femto_interface_monitor_ctx_t;
 
 /* Check if a network interface exists and is UP
  * Returns: 1 if interface is up, 0 otherwise
  */
 int femto_check_interface_available(int if_index);
 
 /* Get the IP address for a specific interface
  * Returns: 0 on success, -1 on failure
  */
 int femto_get_interface_ip(int if_index, struct sockaddr_storage *ip);
 
 /* Get the IPv4 address for a specific interface (IPv4 only)
 * Returns: 0 on success, -1 if no IPv4 address available
 */
 int femto_get_interface_ipv4(int if_index, struct sockaddr_storage *ip);

 /* Get interface name from index
  * Returns: 0 on success, -1 on failure
  */
 int femto_get_interface_name(int if_index, char *ifname, size_t len);
 
 /* Get interface index from name
  * Returns: interface index on success, -1 on failure
  */
 int femto_get_interface_index(const char *ifname);
 
 /* Initialize Netlink socket for interface monitoring
  * Returns: socket fd on success, -1 on failure
  */
 int femto_init_netlink_socket(void);
 
 /* Initialize the interface monitor
  * ctx: monitor context to initialize
  * user_ctx: user context passed to callbacks
  * on_up: callback when interface comes up (can be NULL)
  * on_down: callback when interface goes down (can be NULL)
  * Returns: 0 on success, -1 on failure
  */
 int femto_init_interface_monitor(femto_interface_monitor_ctx_t *ctx,
                                   void *user_ctx,
                                   void (*on_up)(void *, int),
                                   void (*on_down)(void *, int));
 
 /* Initialize the interface monitor with address change callback
  * ctx: monitor context to initialize
  * user_ctx: user context passed to callbacks
  * on_up: callback when interface comes up (can be NULL)
  * on_down: callback when interface goes down (can be NULL)
  * on_addr_change: callback when IP address changes (can be NULL)
  * Returns: 0 on success, -1 on failure
  */
 int femto_init_interface_monitor_full(femto_interface_monitor_ctx_t *ctx,
                                        void *user_ctx,
                                        void (*on_up)(void *, int),
                                        void (*on_down)(void *, int),
                                        void (*on_addr_change)(void *, femto_addr_change_info_t *));
 
 /* Start the interface monitor thread
  * Returns: 0 on success, -1 on failure
  */
 int femto_start_interface_monitor(femto_interface_monitor_ctx_t *ctx);
 
 /* Stop the interface monitor thread
  * Returns: 0 on success, -1 on failure
  */
 int femto_stop_interface_monitor(femto_interface_monitor_ctx_t *ctx);
 
 /* Cleanup the interface monitor */
 void femto_cleanup_interface_monitor(femto_interface_monitor_ctx_t *ctx);
 
 /* List all available network interfaces
  * if_indices: array to store interface indices
  * max_count: maximum number of interfaces to list
  * Returns: number of interfaces found, -1 on failure
  */
 int femto_list_interfaces(int *if_indices, int max_count);
 
 /* Thread function for monitoring interface events (internal use) */
 void *femto_monitor_interface_events(void *arg);
 
 #endif /* FEMTO_INTERFACE_MONITOR_H */
 
 
 
 
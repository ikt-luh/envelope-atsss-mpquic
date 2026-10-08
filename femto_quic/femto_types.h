/*
 * Femto-QUIC, a simple wrapper library for PicoQUIC
 * Based on the PicoQUIC samples by Christian Huitema
 * 
 * Author: David Munstein (2025)
 */

#ifndef FEMTO_TYPES_H
#define FEMTO_TYPES_H

#include <picoquic.h>
#include <picoquic_packet_loop.h>
#include <picoquic_config.h>
#include <pthread.h>
#include "queue.h"
#include <time.h>


#define BUFFER_SIZE 65536
#define MAX_DYNAMIC_PATHS 8

#define FEMTO_STATUS_TYPE_PATH_AVAILABLE 60 + picoquic_callback_path_available
#define FEMTO_STATUS_TYPE_PATH_SUSPENDED 60 + picoquic_callback_path_suspended
#define FEMTO_STATUS_TYPE_PATH_DELETED 60 + picoquic_callback_path_deleted
#define FEMTO_STATUS_TYPE_PATH_QUALITY_CHANGED 60 + picoquic_callback_path_quality_changed


/* === DYNAMIC PATH MANAGEMENT TYPES === */

typedef struct st_dynamic_path_info_t {
    int if_index;                       // interface index (can change on reconnect)
    char if_name[32];                   // interface name (stable across reconnects)
    struct sockaddr_storage server_ip;  // Server IP for this path
    struct sockaddr_storage local_ip;   // Local IP address of this path
    uint64_t unique_path_id;            // PicoQUIC unique path ID (set after probing)
    int is_active;                      // 1 if path is active, 0 if suspended/deleted
    int is_initial;                     // 1 if this is an initial path (not dynamically added)
    int picoquic_path_index;            // Index in PicoQUIC's path array (-1 if unknown)
    time_t added_time;                  // When path was added
} dynamic_path_info_t;


/* === FEMTO CLIENT TYPES === */

typedef struct st_femto_client_stream_ctx_t {
    struct st_femto_client_stream_ctx_t* next_stream;
    uint64_t stream_id;
    uint64_t remote_error;
    unsigned int is_data_sent : 1;
    unsigned int is_stream_reset : 1;
    unsigned int is_stream_finished : 1;
    size_t bytes_received;

    uint8_t *send_data_buffer;  // data to be sent
    size_t send_data_len;  // size of data in data_buffer
    size_t sent_bytes;  // bytes already sent of the data_buffer

    uint8_t *receive_data_buffer;  // buffer for the received data
    size_t receive_buffer_size;  // size for the received buffer
    size_t received_bytes;  // number of bytes received
} femto_client_stream_ctx_t;

typedef struct st_femto_client_ctx_t {
    unsigned int is_closing : 1;
    unsigned int is_closing_requested : 1;
    int is_disconnected;
    int path_id;  // multipathing path ID
    picoquic_cnx_t* cnx;
    femto_client_stream_ctx_t* first_stream;
    femto_client_stream_ctx_t* last_stream;
    void *(*client_callback_fn)(uint8_t *, size_t len, void *);  // called when a data transfer is completed, per client
    void *(*status_callback_fn)(uint8_t, void *, void *);  // called when a status update is received
    void *user_ctx;
    Queue* q;

    struct sockaddr_storage *server_address;
    int *mp_src_if;
    struct sockaddr_storage *mp_alt_ip;
    int mp_nb_alt_paths;
    int mp_init_complete;

    uint32_t active_streams; // number of currently active streams


    // Dynamic path management
    dynamic_path_info_t *mp_dynamic_paths;  // Array tracking dynamically added paths
    int mp_dynamic_path_count;              // Count of dynamically added paths
    pthread_mutex_t mp_path_mutex;          // Mutex for thread-safe path operations
    int mp_monitor_running;                 // Flag to control monitor thread
    pthread_t mp_monitor_thread;            // Interface monitor thread

    
} femto_client_ctx_t;


/* === FEMTO SERVER TYPES === */

typedef struct st_femto_server_stream_ctx_t {
    struct st_femto_server_stream_ctx_t* next_stream;
    struct st_femto_server_stream_ctx_t* previous_stream;
    uint64_t stream_id;
    //uint8_t receive_buffer[BUFFER_SIZE];
    uint8_t* receive_buffer;
    size_t buffer_length;
    unsigned int is_data_read : 1;
    unsigned int is_stream_reset : 1;
    unsigned int is_stream_finished : 1;
    //uint8_t response_buffer[BUFFER_SIZE];
    uint8_t* response_buffer;
    size_t response_size;
    size_t response_sent;

} femto_server_stream_ctx_t;

typedef struct st_femto_server_ctx_t {
    unsigned int is_closing_requested : 1;
    int is_disconnected;
    femto_server_stream_ctx_t* first_stream;
    femto_server_stream_ctx_t* last_stream;
    void *(*server_callback_fn)(uint8_t *, size_t, void *, uint8_t *, size_t *, picoquic_cnx_t *);  // called when data is received completely
    void *(*status_callback_fn)(uint8_t, void *, void *);  // called when a status update is received
    void *user_ctx;
    Queue* q;
} femto_server_ctx_t;


/* === FEMTO TYPES === */

typedef struct st_femto_client {
    char const* alpn;
    char const* sni;
    picoquic_quic_t* quic;
    picoquic_cnx_t* cnx;
    picoquic_quic_config_t* config;
    femto_client_ctx_t* client_ctx;
    picoquic_network_thread_ctx_t* thread_ctx;
    int multipath_enabled;
} femto_client_t;

typedef struct st_femto_server {
    char const* alpn;
    int port;
    picoquic_quic_t* quic;
    picoquic_quic_config_t* config;
    femto_server_ctx_t* server_ctx;
    picoquic_network_thread_ctx_t* thread_ctx;
    pthread_t* loop_thread;
} femto_server_t;

#endif
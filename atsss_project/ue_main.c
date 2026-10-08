/*
 * Smart-Multipath-Project
 * User Equipment (UE) / Client
 * Uses AuToSyndeSis, Femto-QUIC and PicoQUIC
 * 
 * Author: David Munstein (2025)
 */

#include <time.h>
#include "femto_mp.h"
#include "autosyndesis.h"
#include "autosyndesis_actv_stdby.h"
#include "autosyndesis_smallest_delay.h"
#include "autosyndesis_lb_rr.h"
#include "atsss_project.h"

#include "cJSON.h"
#include <curl/curl.h>
#include "curl_functions.h"
#include "femto_interface_monitor.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>

#include <stddef.h>

/* debug toggles */
#ifndef ATSSS_DEBUG_DYNAMIC
#define ATSSS_DEBUG_DYNAMIC 1
#endif

#define MAX_SAMPLES 30
#define MAX_PKGS_IN_FLIGHT 300  // how many packets can be in flight at the same time (sent but not acked)
#define MAX_ACTIVE_STREAMS 3 // how many active PicoQUIC streams can be open at the same time - crash and inacurate measurements if too high

/* global variables */
static int g_path_available[MAX_PATHS] = {0};
static int g_lb_rr_warned = 0;
static int g_probe_log_suppressed[MAX_PATHS] = {0};
/* for local + testbed robustness: reconnect whole femto client for a path (mp_client is multi-cnx).
 * we don't destroy/recreate clients inside the netlink callback thread to avoid races with send_data_mp().
 * instead callbacks set these flags, and the main loop performs the reconnect safely. */
static volatile int g_reconnect_requested[MAX_PATHS] = {0};
static volatile int g_reconnecting[MAX_PATHS] = {0};

static int recreate_mp_client_path(femto_mp_client_t *mp_client, int path_id, void *user_ctx);

/* smoothing / test toggles (overridable via env vars):
 * - ATSSS_DISABLE_APF=1 disables contact apf
 * - ATSSS_DISABLE_RTT_POST=1 disables post_rtt()
 * - ATSSS_PING_INTERVAL_US (default 500000)
 * - ATSSS_PING_TIMEOUT_US (default 1000000)
 * - ATSSS_PING_MISS_THRESHOLD (default 2) number of consecutive missed pongs before marking path down
 */
static int g_disable_apf = 0;
static int g_disable_rtt_post = 0;
static uint64_t g_ping_interval_us = 500000;
static uint64_t g_ping_timeout_us = 1000000;
static int g_ping_miss_threshold = 2;
static uint8_t g_missed_pongs[MAX_PATHS] = {0};

// Struct for the evaliation client context

typedef struct {
    uint64_t timestamp_send;
    uint64_t timestamp_server;
    uint64_t timestamp_received;
    uint32_t seq_num;
    uint8_t path_cts;
    uint8_t path_stc;
    uint8_t autosyndesis_mode;
} mpeval_log_entry_t;

typedef struct {
    int seq_num;  // sequence number
    eval_perf_pkg_t pkg_for_send;  // package to be sent
    eval_perf_pkg_t pkg_received;  // package received
    uint64_t *rtts;  // round-trip times
    uint64_t *arrival_times;  // server arrival times (used for calculating throughput)

    mpeval_log_entry_t *log_struct;
} eval_perf_client_ctx_t;

// Struct for storing 
typedef struct st_ue_cb_ctx_t {
    femto_mp_client_t *mp_client;

    // AuToSyndeSiS path selection algorithms
    autosyndesis_state_t atsss_state;
    autosyndesis_actv_stdby_props_t atsss_actv_stdby_props;
    autosyndesis_minrtt_props_t atsss_minrtt_props;
    autosyndesis_lb_rr_props_t atsss_lb_rr_props;

    // Go-Back-N retransmission
    uint32_t sender_nextseqnum[MAX_CONNECTIONS];
    uint32_t send_base[MAX_CONNECTIONS];
    uint32_t receiver_nextseqnum[MAX_CONNECTIONS];
    uint32_t highest_accepted_seqnum[MAX_CONNECTIONS];  // used for deduplication

    time_t gobackn_time[MAX_CONNECTIONS];
    time_t selrept_times[MAX_CONNECTIONS][GOBACKN_BUFF_SIZE];

    uint8_t *gobackn_buff_ptr[MAX_CONNECTIONS][GOBACKN_BUFF_SIZE];  // also used by selective repeat
    size_t gobackn_buff_size[MAX_CONNECTIONS][GOBACKN_BUFF_SIZE];

    uint8_t gobackn_enabled;  // 0=off 1=Go-Back-N 2=Selective-Repeat

    // To prevent the queue from filling up too much (max packets in flight)
    uint64_t highest_seqnum_acked;

    // Example application: Performance evaluation
    eval_perf_client_ctx_t perf_cli_ctx;

    // Determine path latencies and whether the link is up/down
    uint64_t ping_times[MAX_CONNECTIONS];

    uint8_t active[MAX_CONNECTIONS];
    uint8_t initialized[MAX_CONNECTIONS];
    int connection_id;  // current connection ID assigned by UPF
    int outputfile;

} ue_cb_ctx_t;

/* forward declarations */
static void normalize_lb_rr_shares(ue_cb_ctx_t *ctx, int new_count);
static int choose_available_path(int suggested, int max_paths);
static int count_available_paths(int max_paths);

/**
 * update the scheduler when paths are dynamically added or removed.
 * re-initializes the autosyndesis state with the new path count while
 * preserving the current mode and properties.
 * @param[in] ctx the UE callback context
 * @param[in] new_path_count the new number of paths
 */
 void update_scheduler_paths(ue_cb_ctx_t *ctx, int new_path_count)
 {
     autosyndesis_atsss_mode current_mode = ctx->atsss_state.current_mode;
     void *current_props = ctx->atsss_state.mode_properties;
     
     DEBUG_PRINT("[Scheduler] Updating path count from %d to %d\n", 
            ctx->atsss_state.num_paths, new_path_count);
     
     // re-initialize with new path count
     int ret = autosyndesis_init(&ctx->atsss_state, new_path_count);
     if (ret != 0) {
         fprintf(stderr, "[Scheduler] Failed to re-initialize with %d paths\n", new_path_count);
         return;
     }
    g_lb_rr_warned = 0;
     
     // restore the current mode
     switch (current_mode) {
     case MODE_ACTIVE_STANDBY:
         autosyndesis_set_mode(&ctx->atsss_state, MODE_ACTIVE_STANDBY, &ctx->atsss_actv_stdby_props);
         break;
     case MODE_SMALLEST_DELAY:
         autosyndesis_set_mode(&ctx->atsss_state, MODE_SMALLEST_DELAY, &ctx->atsss_minrtt_props);
         break;
     case MODE_LOAD_BALANCING:
        normalize_lb_rr_shares(ctx, new_path_count);
         autosyndesis_set_mode(&ctx->atsss_state, MODE_LOAD_BALANCING, &ctx->atsss_lb_rr_props);
         break;
     case MODE_MP_DUPLICATION:
         autosyndesis_set_mode(&ctx->atsss_state, MODE_MP_DUPLICATION, NULL);
         break;
     default:
         break;
     }
     
     DEBUG_PRINT("[Scheduler] Path count updated successfully\n");
 }

 
void atsss_upf_set_mode(ue_cb_ctx_t *ue_cb_ctx, uint8_t con_id, uint8_t atsss_mode, uint8_t *parameters, int param_len)
{
    switch (atsss_mode) {
    case ATSSS_MODE_ACTV_STDBY:
        ue_cb_ctx->atsss_actv_stdby_props.default_link = parameters[0];
        autosyndesis_set_mode(&ue_cb_ctx->atsss_state, MODE_ACTIVE_STANDBY, &ue_cb_ctx->atsss_actv_stdby_props);
        DEBUG_PRINT("[DEBUG] AuToSyndeSiS: Set Mode to Active-Standby (default_link=%d)\n", parameters[0]);
        break;
    case ATSSS_MODE_SMAL_DELAY:
        ue_cb_ctx->atsss_minrtt_props.switch_margin_us = (parameters[0] << 24) + (parameters[1] << 16) + (parameters[2] << 8) + parameters[3];
        autosyndesis_set_mode(&ue_cb_ctx->atsss_state, MODE_SMALLEST_DELAY, &ue_cb_ctx->atsss_minrtt_props);
        DEBUG_PRINT("[DEBUG] AuToSyndeSiS: Set Mode to Smallest Delay / MinRTT (margin=%luus)\n", ue_cb_ctx->atsss_minrtt_props.switch_margin_us);
        break;
    case ATSSS_MODE_LB_RR:
        uint8_t num_paths = parameters[0];
        int current_paths = ue_cb_ctx->atsss_state.num_paths;
        if (current_paths < 1) current_paths = 1;
        if (param_len == 1 + num_paths && num_paths == current_paths) {
        memcpy(ue_cb_ctx->atsss_lb_rr_props.share, parameters + 1, num_paths);
        } else {
            normalize_lb_rr_shares(ue_cb_ctx, current_paths);
            if (!g_lb_rr_warned) {
                DEBUG_PRINT("warning: lb_rr set_mode params invalid (len=%d, num_paths=%d), normalized to %d paths\n",
                       param_len, num_paths, current_paths);
                g_lb_rr_warned = 1;
            }
        }
	ue_cb_ctx->atsss_lb_rr_props.change_share = 1;
        autosyndesis_set_mode(&ue_cb_ctx->atsss_state, MODE_LOAD_BALANCING, &ue_cb_ctx->atsss_lb_rr_props);
        DEBUG_PRINT("[DEBUG] AuToSyndeSiS: Set Mode to Load-Balancing (Round Robin)\n");
        break;
    case ATSSS_MODE_MP_DUP:
        ue_cb_ctx->atsss_minrtt_props.switch_margin_us = (parameters[0] << 24) + (parameters[1] << 16) + (parameters[2] << 8) + parameters[3];
        autosyndesis_set_mode(&ue_cb_ctx->atsss_state, MODE_MP_DUPLICATION, &ue_cb_ctx->atsss_minrtt_props);
        DEBUG_PRINT("[DEBUG] AuToSyndeSiS: Set Mode to Multipath Duplication / Opportunistic Redundant (margin=%luus)\n", ue_cb_ctx->atsss_minrtt_props.switch_margin_us);
        break;
    }
}

// Femto-QUIC MP Callback. Called when UE receives data from the UPF on any path
void *mp_client_callback(uint8_t *data, size_t len, void *callback_ctx)
{
    uint64_t receive_time = picoquic_current_time();

    femto_mp_user_ctx_t *user_ctx_ptr = (femto_mp_user_ctx_t *)callback_ctx;
    int path_id = user_ctx_ptr->path_id;
    ue_cb_ctx_t *ue_cb_ctx = (ue_cb_ctx_t *)user_ctx_ptr->user_ctx;
    eval_perf_client_ctx_t *perf_cli_ctx = &ue_cb_ctx->perf_cli_ctx;

    uint8_t temp_buffer[65536];
    uint8_t packet_type, con_id, atsss_mode;
    size_t payload_len;
    uint32_t seq_num;
    int next_path;
    uint64_t timestamp_upf, tuc;

    if (len < 3) {
        DEBUG_PRINT("Received an invalid ATSSS packet (len=%lu)\n", len);
        return NULL;
    }

    if (data[0] != ATSSS_PACKET_MAGIC) {
        DEBUG_PRINT("Received an invalid ATSSS packet magic: 0x%02X\n", data[0]);
        return NULL;
    }

    packet_type = data[1];

    switch (packet_type) {
    /* ATSSS Response: New Connection*/
    case ATSSS_RESP_NEW_CONN:
        con_id = data[2];
        ue_cb_ctx->connection_id = con_id;  // store the connection ID for use in PING packets
        DEBUG_PRINT("Received a new connection response from the server: con_id=%d\n", con_id);
        break;
    /* ATSSS Response: Send Data */
    case ATSSS_RESP_DATA:
        con_id = data[2];
        payload_len = (data[7] << 8) + data[8];
        seq_num = (data[3] << 24) + (data[4] << 16) + (data[5] << 8) + data[6];

       size_t packet_payload_len = ((data[10] << 8) | data[11]) + 3;
        if (payload_len != sizeof(eval_perf_pkg_t) && payload_len != packet_payload_len) {
                DEBUG_PRINT("Received an invalid performance client data packet from the server on path %d (con_id=%d, payload_len=%lu, packet_payload_len=%lu)\n", path_id, con_id, payload_len, packet_payload_len);
        } else if(data[9] == ATSSS_WEBRTC_DATA) {
            //write(ue_cb_ctx->outputfile, data+12, ((data[10] << 8) | data[11]));
            char payload[132];
            memcpy(payload, data+12, payload_len);
            payload[132]='\0';
            DEBUG_PRINT("payload:%s\n", payload);
        }


        // ACK highest sequence number with Go-Back-N logic
        if (ue_cb_ctx->gobackn_enabled == 1) {
            if (seq_num == ue_cb_ctx->receiver_nextseqnum[con_id]) {
                ue_cb_ctx->receiver_nextseqnum[con_id] += 1;
                ue_cb_ctx->highest_accepted_seqnum[con_id] = seq_num;
                // Data was received successfully - handle "upper layer" here
                if (payload_len != sizeof(eval_perf_pkg_t)) {
                    DEBUG_PRINT("Received an invalid performance client data packet from the server on path %d (con_id=%d, payload_len=%lu)\n", path_id, con_id, payload_len);
                } else {
                    eval_perf_pkg_t *pkg_received = (eval_perf_pkg_t *)(data + 9);
                    pkg_received->path_stc = path_id;
                }
            } else {
                seq_num = ue_cb_ctx->receiver_nextseqnum[con_id] - 1;
            }
        } else {
            if (payload_len != sizeof(eval_perf_pkg_t) && payload_len != packet_payload_len) {
                DEBUG_PRINT("Received an invalid performance client data packet from the server on path %d (con_id=%d, payload_len=%lu)\n", path_id, con_id, payload_len);
            } else {
                eval_perf_pkg_t *pkg_received = (eval_perf_pkg_t *)(data + 9);
                pkg_received->path_stc = path_id;
            }
        }

        // Send ACK to UPF
        temp_buffer[0] = ATSSS_PACKET_MAGIC;
        temp_buffer[1] = ATSSS_CMD_DATA_ACK;
        temp_buffer[2] = con_id;
        temp_buffer[3] = seq_num >> 24;
        temp_buffer[4] = seq_num >> 16;
        temp_buffer[5] = seq_num >> 8;
        temp_buffer[6] = seq_num;
        payload_len = 7;

        /* ATSSS Mode: MP Duplication */
        if (ue_cb_ctx->atsss_state.current_mode == MODE_MP_DUPLICATION) {
            // Opportunistic Redundant: Send over all paths if normal transmission
            for (int i = 0; i < ue_cb_ctx->atsss_state.num_paths; i++) {
                int path_idx = choose_available_path(i, ue_cb_ctx->mp_client->num_paths);
                send_data_mp(ue_cb_ctx->mp_client, temp_buffer, payload_len, path_idx);
            }
            DEBUG_PRINT("Sent data ACK to UPF on ALL paths (con_id=%d, seq_num=%u)\n", con_id, seq_num);
            //printf("DEMO01: Seq %lu: Sent ACK to UPF over ALL PATHS. Original DATA was on PATH %d\n", (uint64_t)seq_num, path_id);
            DEBUG_PRINT("DEMO01: UE --[all paths]--> UPF: ACK for Response %d\n", seq_num);
        }
        /* ATSSS Mode: others */
        else {
            next_path = autosyndesis_determine_path(&ue_cb_ctx->atsss_state);
            next_path = choose_available_path(next_path, ue_cb_ctx->mp_client->num_paths);
            send_data_mp(ue_cb_ctx->mp_client, temp_buffer, payload_len, next_path);
            DEBUG_PRINT("Sent data ACK to UPF on path %d (con_id=%d, seq_num=%u)\n", next_path, con_id, seq_num);
            //printf("DEMO01: Seq %lu: Sent ACK to UPF over PATH %d. Original DATA was on PATH %d\n", (uint64_t)seq_num, next_path, path_id);
            DEBUG_PRINT("DEMO01: UE ----[path %d]---> UPF: ACK for Response %d\n", next_path, seq_num);
        }
        break;
    /* ATSSS ACK */
    case ATSSS_CMD_DATA_ACK:
        con_id = data[2];
        seq_num = (data[3] << 24) + (data[4] << 16) + (data[5] << 8) + data[6];

        // To prevent the queue from filling up too much
        if (seq_num > ue_cb_ctx->highest_seqnum_acked) {
            ue_cb_ctx->highest_seqnum_acked = seq_num;
        }

        if (ue_cb_ctx->gobackn_enabled == 1) {  // Go-Back-N
            ue_cb_ctx->send_base[con_id] = seq_num + 1;
            if (ue_cb_ctx->send_base[con_id] == ue_cb_ctx->sender_nextseqnum[con_id]) {
                ue_cb_ctx->gobackn_time[con_id] = time(NULL);
            }

            // free Go-Back-N buffer entry as data was acknowledged
            if (ue_cb_ctx->gobackn_buff_ptr[con_id][seq_num % GOBACKN_BUFF_SIZE] == NULL) {
                DEBUG_PRINT("Received an invalid data ACK from the UPF on path %i (con_id=%d, seq_num=%u).\n", path_id, con_id, seq_num);
            } else {
                DEBUG_PRINT("Received data ACK from the UPF on path %i (con_id=%d, seq_num=%u)\n", path_id, con_id, seq_num);
                free(ue_cb_ctx->gobackn_buff_ptr[con_id][seq_num % GOBACKN_BUFF_SIZE]);
                ue_cb_ctx->gobackn_buff_ptr[con_id][seq_num % GOBACKN_BUFF_SIZE] = NULL;
                ue_cb_ctx->gobackn_buff_size[con_id][seq_num % GOBACKN_BUFF_SIZE] = 0;
            }
        } else if (ue_cb_ctx->gobackn_enabled == 2) {  // Selective Repeat
            // free Go-Back-N buffer entry as data was acknowledged
            if (ue_cb_ctx->gobackn_buff_ptr[con_id][seq_num % GOBACKN_BUFF_SIZE] == NULL) {
                DEBUG_PRINT("Received an invalid data ACK from the UPF on path %i (con_id=%d, seq_num=%u).\n", path_id, con_id, seq_num);
            } else {
                DEBUG_PRINT("Received data ACK from the UPF on path %i (con_id=%d, seq_num=%u)\n", path_id, con_id, seq_num);
                free(ue_cb_ctx->gobackn_buff_ptr[con_id][seq_num % GOBACKN_BUFF_SIZE]);
                ue_cb_ctx->gobackn_buff_ptr[con_id][seq_num % GOBACKN_BUFF_SIZE] = NULL;
                ue_cb_ctx->gobackn_buff_size[con_id][seq_num % GOBACKN_BUFF_SIZE] = 0;
            }

            // move the send base to the next unacknowledged packet
            while (ue_cb_ctx->send_base[con_id] < ue_cb_ctx->sender_nextseqnum[con_id] &&
                   ue_cb_ctx->gobackn_buff_ptr[con_id][ue_cb_ctx->send_base[con_id] % GOBACKN_BUFF_SIZE] == NULL) {
                ue_cb_ctx->send_base[con_id] += 1;
            }
        } else {
            DEBUG_PRINT("Received data ACK from the UPF on path %i (con_id=%d, seq_num=%u)\n", path_id, con_id, seq_num);
        }
        break;
    /* ATSSS CMD: Ping*/
    case ATSSS_CMD_PING:
        if (len < 11) {
            DEBUG_PRINT("Invalid ATSSS packet received (len=%lu) for init path\n", len);
            return NULL;
        }
        /* during per-path reconnect we destroy/recreate the femto_client for that path.
         * avoid sending on a client that may be mid-destruction. */
        if (path_id >= 0 && path_id < MAX_PATHS && g_reconnecting[path_id]) {
            if (ATSSS_DEBUG_DYNAMIC) {
                DEBUG_PRINT("[debug][dyn] dropping ping while reconnecting: path_id=%d\n", path_id);
            }
            break;
        }
        if (ATSSS_DEBUG_DYNAMIC) {
            DEBUG_PRINT("[debug][dyn] ping received: path_id=%d, len=%lu, scheduler_num_paths=%d, mp_num_paths=%d, available=%d\n",
                   path_id, len, ue_cb_ctx->atsss_state.num_paths, ue_cb_ctx->mp_client->num_paths,
                   (path_id >= 0 && path_id < MAX_PATHS) ? g_path_available[path_id] : -1);
        }
        timestamp_upf = ((uint64_t)data[3] << 56) + ((uint64_t)data[4] << 48) + ((uint64_t)data[5] << 40) + ((uint64_t)data[6] << 32) +
                                 ((uint64_t)data[7] << 24) + ((uint64_t)data[8] << 16) + ((uint64_t)data[9] << 8) + (uint64_t)data[10];
        tuc = receive_time - timestamp_upf;
        // tuc is one-way delay which requires synchronized clocks. Don't use it
        //if (timestamp_upf > 0) {
        //    autosyndesis_update_link_properties(&ue_cb_ctx->atsss_state, path_id, tuc);
        //    DEBUG_PRINT("Updated link properties for path %d via CMD_PING cmd: RTT=%luus\n", path_id, tuc);
        //}
        /* always reply on the same path the ping came in on.
         * use a local buffer so we do not depend on the lifetime of the input packet memory. */
        uint8_t pong_buf[32];
        if (len > sizeof(pong_buf)) {
            DEBUG_PRINT("[AUTO] ping packet too large to pong (len=%lu) on path %d\n", len, path_id);
            return NULL;
        }
        memcpy(pong_buf, data, len);
        pong_buf[1] = ATSSS_CMD_PONG;
        pong_buf[3] = timestamp_upf >> 56; pong_buf[4] = timestamp_upf >> 48;
        pong_buf[5] = timestamp_upf >> 40; pong_buf[6] = timestamp_upf >> 32;
        pong_buf[7] = timestamp_upf >> 24; pong_buf[8] = timestamp_upf >> 16;
        pong_buf[9] = timestamp_upf >> 8;  pong_buf[10] = timestamp_upf;
        if (ATSSS_DEBUG_DYNAMIC && path_id < MAX_PATHS && !g_path_available[path_id]) {
            DEBUG_PRINT("[debug][dyn] replying pong on currently unavailable path %d\n", path_id);
        }
        {
            int rc = send_data_mp(ue_cb_ctx->mp_client, pong_buf, len, path_id);
            if (ATSSS_DEBUG_DYNAMIC) {
                DEBUG_PRINT("[debug][dyn] pong sent in response to ping: path_id=%d, rc=%d\n", path_id, rc);
            }
            if (rc != 0) {
                DEBUG_PRINT("[AUTO] warning: failed to send pong on path %d (rc=%d)\n", path_id, rc);
            }
        }
        break;
    /* ATSSS CMD: Pong*/
    case ATSSS_CMD_PONG:
        if (len < 11) {
            DEBUG_PRINT("Invalid ATSSS packet received (len=%lu) for init path\n", len);
            return NULL;
        }
        timestamp_upf = ((uint64_t)data[3] << 56) + ((uint64_t)data[4] << 48) + ((uint64_t)data[5] << 40) + ((uint64_t)data[6] << 32) +
                                 ((uint64_t)data[7] << 24) + ((uint64_t)data[8] << 16) + ((uint64_t)data[9] << 8) + (uint64_t)data[10];
        //tuc = receive_time - timestamp_upf;
        if (ue_cb_ctx->ping_times[path_id] == 0) {
            DEBUG_PRINT("Received PONG without a corresponding PING on path %d\n", path_id);
            break;
        }
        tuc = receive_time - ue_cb_ctx->ping_times[path_id];
        ue_cb_ctx->ping_times[path_id] = 0;
        if (path_id >= 0 && path_id < MAX_PATHS) {
            g_missed_pongs[path_id] = 0;
        }
        if (timestamp_upf > 0) {
            if (ATSSS_DEBUG_DYNAMIC) {
                DEBUG_PRINT("[debug][dyn] pong received: path_id=%d, scheduler_num_paths(before)=%d, mp_num_paths=%d, available=%d\n",
                       path_id, ue_cb_ctx->atsss_state.num_paths, ue_cb_ctx->mp_client->num_paths,
                       (path_id >= 0 && path_id < MAX_PATHS) ? g_path_available[path_id] : -1);
            }
            /* if this pong indicates a previously unavailable path is back, update availability
             * and expand the scheduler first so autosyndesis sees a valid path id. */
            if (path_id < MAX_PATHS && !g_path_available[path_id]) {
                g_path_available[path_id] = 1;
                g_probe_log_suppressed[path_id] = 0;
                DEBUG_PRINT("[AUTO] path re-enabled after pong on path %d\n", path_id);
            }

            /* do not shrink scheduler path count during flaps.
             * available paths are selected via g_path_available[] + choose_available_path(). */

            if (path_id < ue_cb_ctx->atsss_state.num_paths) {
                autosyndesis_update_link_properties(&ue_cb_ctx->atsss_state, path_id, tuc);
                DEBUG_PRINT("Updated link properties for path %d via CMD_PONG cmd: RTT=%luus\n", path_id, tuc);
            } else if (ATSSS_DEBUG_DYNAMIC) {
                DEBUG_PRINT("[debug][dyn] skipping autosyndesis update: path_id=%d, scheduler_num_paths=%d\n",
                       path_id, ue_cb_ctx->atsss_state.num_paths);
            }
        }
        break;
    }
}


/**
 * Create a new connection over the ATSSS multipath connection
 * @param mp_client the multipath client structure
 * @param path_id the path ID
 * @param addr the address of the target application server. E.g., "10.0.0.1:4443"
 * @param addr_len the length of the address
 */
void atsss_new_connection(ue_cb_ctx_t *ue_cb_ctx, int path_id, char *addr, size_t addr_len)
{
    size_t send_len;
    uint8_t send_buffer[256];

    if (addr_len > 256 - 4) {
        DEBUG_PRINT("Address too large for ATSSS new connection: %lu\n", addr_len);
        return;
    }

    send_buffer[0] = ATSSS_PACKET_MAGIC;
    send_buffer[1] = ATSSS_CMD_NEW_CONN;
    send_buffer[2] = addr_len;
    memcpy(send_buffer + 3, addr, addr_len);
    send_len = 3 + addr_len;

    ue_cb_ctx->active[path_id] = 1;
    ue_cb_ctx->initialized[path_id] = 1;

    send_data_mp(ue_cb_ctx->mp_client, (char *)send_buffer, send_len, path_id);
}

/**
 * Close an existing connection over the ATSSS multipath connection
 * @param mp_client the multipath client structure
 * @param path_id the path ID
 * @param con_id the connection ID
 */
void atsss_close_connection(ue_cb_ctx_t *ue_cb_ctx, int path_id, int con_id)
{
    size_t send_len;
    uint8_t send_buffer[3];

    send_buffer[0] = ATSSS_PACKET_MAGIC;
    send_buffer[1] = ATSSS_CMD_CLOSE_CONN;
    send_buffer[2] = con_id;
    send_len = 3;

    ue_cb_ctx->active[path_id] = 0;
    ue_cb_ctx->initialized[path_id] = 0;

    send_data_mp(ue_cb_ctx->mp_client, (char *)send_buffer, send_len, path_id);

}

/**
 * Send data over an existing connection over the ATSSS multipath connection
 * @param mp_client the multipath client structure
 * @param path_id the path ID
 * @param con_id the connection ID
 * @param seq_num the sequence number
 * @param data the data to send
 * @param len the length of the data
 */
static int atsss_send_data(femto_mp_client_t *mp_client, int path_id, int con_id, uint32_t seq_num, char *data, size_t data_len)
{
    size_t send_len;
    uint8_t send_buffer[65536];

    if (data_len > 65536 - 5) {
        DEBUG_PRINT("Data too large for ATSSS send: %lu\n", data_len);
        return -1;
    }

    send_buffer[0] = ATSSS_PACKET_MAGIC;
    send_buffer[1] = ATSSS_CMD_SEND_DATA;
    send_buffer[2] = con_id;
    send_buffer[3] = seq_num >> 24;
    send_buffer[4] = seq_num >> 16;
    send_buffer[5] = seq_num >> 8;
    send_buffer[6] = seq_num;
    send_buffer[7] = data_len >> 8;
    send_buffer[8] = data_len;
    memcpy(send_buffer + ATSSS_SEND_DATA_HEADER_LEN, data, data_len);
    send_len = ATSSS_SEND_DATA_HEADER_LEN + data_len;

    return send_data_mp(mp_client, (char *)send_buffer, send_len, path_id);
}

void contact_aue(ue_cb_ctx_t *ue_cb_ctx, int num_paths){
    double ratio_wifi = 50;
    double ratio_5g = 50;
    const char *ratio_url = getenv("AUE_RATIO_URL");
    if (!ratio_url || ratio_url[0] == '\0') {
        ratio_url = "http://127.0.0.1:8101/ratio";  // default
    }
    int ret = get_ratio(ratio_url, &ratio_5g);
    DEBUG_PRINT("FIVEG:%f\n", ratio_5g);
    if(ret != 0){
        DEBUG_PRINT("GETTING RATIOS DIDN'T WORK\n");
    }else {
	ue_cb_ctx->atsss_lb_rr_props.change_share = 1;
        uint8_t shares[] = {ratio_5g, 100 - ratio_5g};
        memcpy(ue_cb_ctx->atsss_lb_rr_props.share, shares, num_paths);
        autosyndesis_set_mode(&ue_cb_ctx->atsss_state, MODE_LOAD_BALANCING, &ue_cb_ctx->atsss_lb_rr_props);
    }
}

// global pointer for interface monitor callbacks
static ue_cb_ctx_t *g_ue_cb_ctx = NULL;
static char g_server_ip[64] = "";   // default server IP for truly new interfaces
static int g_server_port = 0;       // default server port for truly new interfaces

static void normalize_lb_rr_shares(ue_cb_ctx_t *ctx, int new_count)
{
    if (new_count < 1) {
        new_count = 1;
    }
    for (int i = 0; i < MAX_PATHS; i++) {
        ctx->atsss_lb_rr_props.share[i] = (i < new_count) ? 1 : 0;
    }
}

static int choose_available_path(int suggested, int max_paths)
{
    if (max_paths < 1) {
        return 0;
    }
    int base = suggested % max_paths;
    for (int i = 0; i < max_paths; i++) {
        int candidate = (base + i) % max_paths;
        if (candidate >= 0 && candidate < MAX_PATHS && g_path_available[candidate]) {
            return candidate;
        }
    }
    return base;
}

static int count_available_paths(int max_paths)
{
    int available = 0;
    int limit = (max_paths > MAX_PATHS) ? MAX_PATHS : max_paths;
    for (int i = 0; i < limit; i++) {
        if (g_path_available[i]) {
            available++;
        }
    }
    if (available < 1) {
        available = 1;
    }
    return available;
}

/**
 * retry pending path probes that haven't been confirmed after a timeout.
 * this handles the case where an initial probe fails due to interface flapping.
 * @param[in] mp_client the multipath client
 * @param[in] retry_timeout_sec timeout in seconds before retrying a probe
 */
static void retry_pending_path_probes(femto_mp_client_t *mp_client, int retry_timeout_sec)
{
    time_t now = time(NULL);
    static time_t last_log = 0;
    static int last_reported_count = -1;
    int found_pending = 0;
    int total_paths = 0;
    int inactive_count = 0;
    
    for (int c = 0; c < mp_client->num_paths; c++) {
        femto_client_ctx_t *client_ctx = mp_client->clients[c].client_ctx;
        if (client_ctx == NULL) continue;
        
        pthread_mutex_lock(&client_ctx->mp_path_mutex);
        
        // debug: log path counts
        total_paths += client_ctx->mp_dynamic_path_count;
        for (int i = 0; i < client_ctx->mp_dynamic_path_count; i++) {
            if (!client_ctx->mp_dynamic_paths[i].is_active) {
                inactive_count++;
            }
        }
        
        // log pending paths periodically (every 10 seconds) when there are inactive paths
        if (now - last_log > 10 && inactive_count > 0) {
            DEBUG_PRINT("[AUTO-Retry] Client %d: %d/%d paths inactive\n", 
                   c, inactive_count, client_ctx->mp_dynamic_path_count);
            last_log = now;
        }
        
        for (int i = 0; i < client_ctx->mp_dynamic_path_count; i++) {
            dynamic_path_info_t *path = &client_ctx->mp_dynamic_paths[i];
            
            // check for pending probes: path exists, not active, and timed out
            if (!path->is_active && path->added_time > 0) {
                found_pending = 1;
                time_t age = now - path->added_time;
                
                if (age <= retry_timeout_sec) {
                    // not yet time to retry
                    continue;
                }
                
                // check if interface is now available
                int if_available = femto_check_interface_available(path->if_index);
                if (!if_available) {
                    if (now - last_log > 5) {
                        DEBUG_PRINT("[AUTO-Retry] Interface %d (%s) not available yet (age=%lds)\n", 
                               path->if_index, path->if_name, age);
                        last_log = now;
                    }
                    continue;
                }
                
                struct sockaddr_storage local_addr;
                if (femto_get_interface_ip(path->if_index, &local_addr) != 0) {
                    if (now - last_log > 5) {
                        DEBUG_PRINT("[AUTO-Retry] Can't get IP for interface %d (%s)\n", 
                               path->if_index, path->if_name);
                        last_log = now;
                    }
                    continue;
                }
                
                // skip if only IPv6 link-local
                if (local_addr.ss_family == AF_INET6) {
                    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&local_addr;
                    if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr)) {
                        if (now - last_log > 5) {
                            DEBUG_PRINT("[AUTO-Retry] Interface %d has only IPv6 link-local\n", 
                                   path->if_index);
                            last_log = now;
                        }
                        continue;
                    }
                }
                
                // got a valid IPv4 address, retry the probe
                char ip_str[INET_ADDRSTRLEN];
                struct sockaddr_in *addr4 = (struct sockaddr_in *)&local_addr;
                inet_ntop(AF_INET, &addr4->sin_addr, ip_str, sizeof(ip_str));
                
                DEBUG_PRINT("[AUTO] Retrying pending path probe for interface %d (%s) with IP %s\n", 
                       path->if_index, path->if_name, ip_str);
                
                // re-probe the path
                int ret = picoquic_probe_new_path_ex(
                    mp_client->clients[c].cnx,
                    (struct sockaddr*)&path->server_ip,
                    (struct sockaddr*)&local_addr,
                    path->if_index,
                    picoquic_get_quic_time(mp_client->clients[c].quic),
                    0);
                
                if (ret == 0) {
                    path->added_time = now;  // reset timer for next retry
                    DEBUG_PRINT("[AUTO] Path probe retry initiated for interface %d\n", path->if_index);
                } else {
                    DEBUG_PRINT("[AUTO] Path probe retry failed for interface %d (error=%d)\n", 
                           path->if_index, ret);
                }
            }
        }
        
        pthread_mutex_unlock(&client_ctx->mp_path_mutex);
    }
}

static int total_active_paths_clamped(femto_mp_client_t *mp_client)
{
    if (mp_client == NULL) {
        return 0;
    }
    int total_active = 0;
    for (int i = 0; i < mp_client->num_paths; i++) {
        total_active += femto_get_active_path_count(&mp_client->clients[i]);
    }
    if (total_active > mp_client->num_paths) {
        total_active = mp_client->num_paths;
    }
    return total_active;
}

// Helper function: find which client owns a given interface index
// Returns the client index (0 to num_paths-1), or -1 if not found
static int find_client_for_interface(femto_mp_client_t *mp_client, int if_index)
{
    // Check by interface index first
    for (int i = 0; i < mp_client->num_paths; i++) {
        if (mp_client->server_ifs[i] == if_index) {
            return i;
        }
    }
    
    // Also check by interface name (handles index changes on reconnect)
    char if_name[32];
    if (femto_get_interface_name(if_index, if_name, sizeof(if_name)) == 0) {
        for (int i = 0; i < mp_client->num_paths; i++) {
            // Check if this client's dynamic path tracking has this interface
            femto_client_ctx_t *ctx = mp_client->clients[i].client_ctx;
            if (ctx != NULL && ctx->mp_dynamic_paths != NULL) {
                for (int j = 0; j < ctx->mp_dynamic_path_count; j++) {
                    if (strcmp(ctx->mp_dynamic_paths[j].if_name, if_name) == 0) {
                        return i;
                    }
                }
            }
        }
    }
    
    return -1;
}

/* recreate the Femto client for one mp_client path.
 * this is needed because femto_mp_client is implemented as multiple independent QUIC connections (one per path),
 * so a link flap can permanently disconnect that path's connection.
 *
 * important: must be called from the main thread/loop (not from netlink callbacks) to avoid races with send_data_mp().
 */
static int recreate_mp_client_path(femto_mp_client_t *mp_client, int path_id, void *user_ctx)
{
    if (mp_client == NULL || mp_client->clients == NULL) {
        return -1;
    }
    if (path_id < 0 || path_id >= mp_client->num_paths) {
        return -1;
    }

    char server_name[MAX_SERVER_NAME_LENGTH * 2];
    snprintf(server_name, sizeof(server_name), "%s:%d/%d",
             mp_client->server_names[path_id],
             mp_client->server_ports[path_id],
             mp_client->server_ifs[path_id]);

    /* important:
     * destroy_femto_client() is currently not safe to call while the program is actively exchanging packets
     * (race with picoquic network threads). it can segfault during teardown.
     *
     * for robustness (and to make local testing possible), we create a fresh femto_client_t and overwrite the slot.
     * this leaks the old client instance, but avoids crashes and restores the path.
     * if we need long-running stability, we should refactor femto-quic to support safe in-place reconnect/teardown.
     */
    femto_client_t new_client;
    memset(&new_client, 0, sizeof(new_client));

    femto_mp_user_ctx_t *user_ctx_ptr = (femto_mp_user_ctx_t *)malloc(sizeof(femto_mp_user_ctx_t));
    if (user_ctx_ptr == NULL) {
        return -1;
    }
    user_ctx_ptr->path_id = path_id;
    user_ctx_ptr->user_ctx = user_ctx;

    char if_name_str[16];
    sprintf(if_name_str, "%d", mp_client->server_ifs[path_id]);

    int rc = create_femto_client_bind_if(&new_client,
                                         server_name,
                                         ATSSS_UE_UPF_ALPN,
                                         ATSSS_UE_UPF_SNI,
                                         &mp_client_callback,
                                         user_ctx_ptr,
                                         if_name_str);
    if (rc != 0) {
        free(user_ctx_ptr);
        return rc;
    }

    mp_client->clients[path_id] = new_client;
    return 0;
}

// callback: called automatically when an interface comes UP
// note: we don't immediately add a path here, we wait for an IP address to be assigned to the interface
// the on_addr_change_callback will handle the path addition when we get a valid IPv4.
void on_interface_up_callback(void *ctx, int if_index)
{
    if (g_ue_cb_ctx == NULL || g_ue_cb_ctx->mp_client == NULL) return;

    if (ATSSS_DEBUG_DYNAMIC) {
        DEBUG_PRINT("[debug][dyn] interface up: if_index=%d, scheduler_num_paths=%d, mp_num_paths=%d\n",
               if_index, g_ue_cb_ctx->atsss_state.num_paths, g_ue_cb_ctx->mp_client->num_paths);
        for (int i = 0; i < g_ue_cb_ctx->mp_client->num_paths; i++) {
            DEBUG_PRINT("[debug][dyn] mp map: idx=%d, server_if=%d, server=%s:%d, available=%d\n",
                   i,
                   g_ue_cb_ctx->mp_client->server_ifs[i],
                   g_ue_cb_ctx->mp_client->server_names[i],
                   g_ue_cb_ctx->mp_client->server_ports[i],
                   (i < MAX_PATHS) ? g_path_available[i] : -1);
        }
    }
    
    DEBUG_PRINT("\n[AUTO] Interface %d came UP - waiting for IP address assignment...\n", if_index);

    // don't add immediately, wait for an IP address to be assigned to the interface



    /* for mp_client (multi-cnx), we need to reconnect the owning client when its interface is back.
     * only request reconnect here; the main loop performs it safely. */
    int owning_client = find_client_for_interface(g_ue_cb_ctx->mp_client, if_index);
    if (owning_client >= 0 && owning_client < MAX_PATHS) {
        g_ue_cb_ctx->mp_client->server_ifs[owning_client] = if_index; /* update in case index changed */
        g_reconnect_requested[owning_client] = 1;
        DEBUG_PRINT("[AUTO] Interface %d belongs to client %d -> requested path reconnect\n", if_index, owning_client);
        g_probe_log_suppressed[owning_client] = 0;
    }
}

// Callback: Called automatically when an interface goes DOWN
void on_interface_down_callback(void *ctx, int if_index)
{
    if (g_ue_cb_ctx == NULL || g_ue_cb_ctx->mp_client == NULL) return;

    if (ATSSS_DEBUG_DYNAMIC) {
        DEBUG_PRINT("[debug][dyn] interface down: if_index=%d, scheduler_num_paths=%d, mp_num_paths=%d\n",
               if_index, g_ue_cb_ctx->atsss_state.num_paths, g_ue_cb_ctx->mp_client->num_paths);
        for (int i = 0; i < g_ue_cb_ctx->mp_client->num_paths; i++) {
            DEBUG_PRINT("[debug][dyn] mp map: idx=%d, server_if=%d, server=%s:%d, available=%d\n",
                   i,
                   g_ue_cb_ctx->mp_client->server_ifs[i],
                   g_ue_cb_ctx->mp_client->server_names[i],
                   g_ue_cb_ctx->mp_client->server_ports[i],
                   (i < MAX_PATHS) ? g_path_available[i] : -1);
        }
    }
    
    DEBUG_PRINT("\n[AUTO] Interface %d went DOWN - attempting to remove path...\n", if_index);
    
    // Search through all clients to find which one has this interface
    int ret = -1;
    int removed_from_client = -1;
    for (int i = 0; i < g_ue_cb_ctx->mp_client->num_paths; i++) {
        ret = femto_remove_path_dynamic(&g_ue_cb_ctx->mp_client->clients[i], if_index);
        if (ret == 0) {
            removed_from_client = i;
            break;
        }
    }
    
    if (ret == 0) {
        DEBUG_PRINT("[AUTO] Successfully removed path on interface %d (from client %d)\n", if_index, removed_from_client);
        g_path_available[removed_from_client] = 0;
        if (removed_from_client >= 0 && removed_from_client < MAX_PATHS) {
            g_probe_log_suppressed[removed_from_client] = 0;
        }
        /* do not shrink scheduler path count during flaps.
         * available paths are selected via g_path_available[] + choose_available_path(). */
    } else {
        DEBUG_PRINT("[AUTO] No path to remove for interface %d (error=%d)\n", if_index, ret);
    }
}

// callback: called automatically when an interface's IP address changes
// this handles provider handover (e.g., 5G Provider X -> Provider Y)
// also handles initial IPv4 assignment after interface comes up
void on_addr_change_callback(void *ctx, femto_addr_change_info_t *change_info)
{
    if (g_ue_cb_ctx == NULL || g_ue_cb_ctx->mp_client == NULL) return;
    if (change_info == NULL) return;

    /* if IPv4 was removed from the interface (ADDR_DEL), mark the owning path unavailable immediately.
     * this avoids ~1s of "stale" scheduling where data is still sent on the down path until ping timeout. */
    if (!change_info->has_new_ip && change_info->has_old_ip) {
        if (change_info->old_ip.ss_family == AF_INET6) {
            struct sockaddr_in6 *old_addr6 = (struct sockaddr_in6 *)&change_info->old_ip;
            if (IN6_IS_ADDR_LINKLOCAL(&old_addr6->sin6_addr)) {
                /* ignore IPv6 link-local deletion; we steer based on IPv4 availability */
                return;
            }
        }

        if (change_info->old_ip.ss_family == AF_INET) {
            char old_ip_str[INET_ADDRSTRLEN];
            struct sockaddr_in *old_addr4 = (struct sockaddr_in *)&change_info->old_ip;
            inet_ntop(AF_INET, &old_addr4->sin_addr, old_ip_str, sizeof(old_ip_str));

            int owning_client = find_client_for_interface(g_ue_cb_ctx->mp_client, change_info->if_index);
            if (owning_client >= 0 && owning_client < MAX_PATHS) {
                DEBUG_PRINT("[AUTO] Interface %d lost IPv4 (%s) -> marking path %d unavailable\n",
                       change_info->if_index, old_ip_str, owning_client);
                g_path_available[owning_client] = 0;
                g_missed_pongs[owning_client] = 0;
                g_probe_log_suppressed[owning_client] = 0;
                g_ue_cb_ctx->ping_times[owning_client] = 0;
                g_reconnect_requested[owning_client] = 1;

                /* do not shrink scheduler path count during flaps.
                 * available paths are selected via g_path_available[] + choose_available_path(). */
            }
        }
        return;
    }

    // Check if the new IP is IPv4 (we only want to act on IPv4 changes)
    if (change_info->has_new_ip && change_info->new_ip.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&change_info->new_ip;
        if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr)) {
            // Ignore IPv6 link-local address changes - wait for IPv4
            DEBUG_PRINT("[AUTO] Interface %d got IPv6 link-local - ignoring, waiting for IPv4\n", 
                   change_info->if_index);
            return;
        }
    }
    
    // Check if old IP was IPv6 link-local and new IP is IPv4 - this is initial assignment
    int is_initial_ipv4 = 0;
    if (change_info->has_old_ip && change_info->old_ip.ss_family == AF_INET6) {
        struct sockaddr_in6 *old_addr6 = (struct sockaddr_in6 *)&change_info->old_ip;
        if (IN6_IS_ADDR_LINKLOCAL(&old_addr6->sin6_addr)) {
            if (change_info->has_new_ip && change_info->new_ip.ss_family == AF_INET) {
                is_initial_ipv4 = 1;
            }
        }
    }
    
    if (is_initial_ipv4) {
        char new_ip_str[INET_ADDRSTRLEN];
        struct sockaddr_in *new_addr4 = (struct sockaddr_in *)&change_info->new_ip;
        inet_ntop(AF_INET, &new_addr4->sin_addr, new_ip_str, sizeof(new_ip_str));

        if (ATSSS_DEBUG_DYNAMIC) {
            DEBUG_PRINT("[debug][dyn] addr change initial ipv4: if_index=%d, ip=%s, scheduler_num_paths=%d, mp_num_paths=%d\n",
                   change_info->if_index, new_ip_str,
                   g_ue_cb_ctx->atsss_state.num_paths, g_ue_cb_ctx->mp_client->num_paths);
        }
        
        DEBUG_PRINT("\n[AUTO] Interface %d got IPv4 address %s - adding path...\n", 
               change_info->if_index, new_ip_str);
        
        int owning_client = find_client_for_interface(g_ue_cb_ctx->mp_client, change_info->if_index);
        if (owning_client >= 0 && owning_client < MAX_PATHS) {
            g_ue_cb_ctx->mp_client->server_ifs[owning_client] = change_info->if_index;
            g_reconnect_requested[owning_client] = 1;
            DEBUG_PRINT("[AUTO] Interface %d got IPv4; client %d -> requested path reconnect\n",
                   change_info->if_index, owning_client);
            g_probe_log_suppressed[owning_client] = 0;
        }
        return;
    }
    
    // this is a real IP change (IPv4 -> IPv4 or IPv6 global -> IPv6 global)
    
    DEBUG_PRINT("\n[AUTO] Interface %d IP address changed - migrating path...\n", change_info->if_index);
    
    // Find which client owns this interface
    int owning_client = find_client_for_interface(g_ue_cb_ctx->mp_client, change_info->if_index);
    
    // Step 1: remove the old path from the owning client
    int removed_from_client = -1;
    if (owning_client >= 0) {
        int ret = femto_remove_path_dynamic(&g_ue_cb_ctx->mp_client->clients[owning_client], change_info->if_index);
    if (ret == 0) {
            removed_from_client = owning_client;
            DEBUG_PRINT("[AUTO] Removed old path on interface %d (from client %d)\n", change_info->if_index, owning_client);
    }
    } else {
        // Search all clients if owning client not found
        for (int i = 0; i < g_ue_cb_ctx->mp_client->num_paths; i++) {
            int ret = femto_remove_path_dynamic(&g_ue_cb_ctx->mp_client->clients[i], change_info->if_index);
            if (ret == 0) {
                removed_from_client = i;
                owning_client = i;
                DEBUG_PRINT("[AUTO] Removed old path on interface %d (from client %d)\n", change_info->if_index, i);
                break;
            }
        }
    }
    
    // Step 2: add a new path with the new IP to the owning client
    int ret = -1;
    if (owning_client >= 0) {
        const char *server_ip = g_ue_cb_ctx->mp_client->server_names[owning_client];
        int server_port = g_ue_cb_ctx->mp_client->server_ports[owning_client];
        
        ret = femto_add_path_dynamic(&g_ue_cb_ctx->mp_client->clients[owning_client], 
                                      change_info->if_index, server_ip, server_port);
    } else {
        // Fallback to default server
    ret = femto_add_path_dynamic(&g_ue_cb_ctx->mp_client->clients[0], 
                                  change_info->if_index, g_server_ip, g_server_port);
        owning_client = 0;
    }
    
    if (ret == 0) {
        DEBUG_PRINT("[AUTO] Successfully migrated path on interface %d to new IP (to client %d)\n", 
               change_info->if_index, owning_client);
        DEBUG_PRINT("[AUTO] path re-enable pending confirmation for client %d (interface %d) after migration\n", owning_client, change_info->if_index);
    } else if (ret == -3) {
        DEBUG_PRINT("[AUTO] Path for interface %d already exists after migration\n", change_info->if_index);
    } else {
        DEBUG_PRINT("[AUTO] Failed to add new path after IP change (error=%d)\n", ret);
        // Path count may have decreased - don't update scheduler since we're re-adding, not adding new
        int active_count = 0;
        for (int i = 0; i < g_ue_cb_ctx->mp_client->num_paths; i++) {
            active_count += femto_get_active_path_count(&g_ue_cb_ctx->mp_client->clients[i]);
        }
        if (active_count < g_ue_cb_ctx->atsss_state.num_paths && active_count >= 1) {
            update_scheduler_paths(g_ue_cb_ctx, active_count);
        }
    }
}

int main(int argc, char **argv)
{
    int ret;
    int next_path;
    int num_iters = 1000;  // number of iterations for the performance test
    int max_stream_count_limit_reached;
    uint32_t highest_seq_logged = 0;  // highest sequence number logged in the log file + 1
    uint64_t sending_interval_us = 1000;  // interval between sending packets in microseconds
    femto_mp_client_t mp_client;
    ue_cb_ctx_t ue_cb_ctx = { 0 };
    ue_cb_ctx.mp_client = &mp_client;

    // interface monitor for automatic path management
    femto_interface_monitor_ctx_t interface_monitor;



    /* env-based knobs for local testing / smoothing */
    {
        const char *v;
        v = getenv("ATSSS_DISABLE_APF");
        if (v != NULL && atoi(v) != 0) g_disable_apf = 1;
        v = getenv("ATSSS_DISABLE_RTT_POST");
        if (v != NULL && atoi(v) != 0) g_disable_rtt_post = 1;

        v = getenv("ATSSS_PING_INTERVAL_US");
        if (v != NULL && atoll(v) > 0) g_ping_interval_us = (uint64_t)atoll(v);
        v = getenv("ATSSS_PING_TIMEOUT_US");
        if (v != NULL && atoll(v) > 0) g_ping_timeout_us = (uint64_t)atoll(v);
        v = getenv("ATSSS_PING_MISS_THRESHOLD");
        if (v != NULL && atoi(v) > 0) g_ping_miss_threshold = atoi(v);
    }

    if (argc != 3 && argc != 8 && argc != 11) {
        fprintf(stderr, "ATSSS UE Usage: %s <server_list> <destination app server> [<initial mode> <mode parameters> <num iterations> <gobackn enabled> <sending interval>] [<fifo_output> <fifo_input>]\n", argv[0]);
        return 1;
    }
    DEBUG_PRINT("ATSSS UE: Starting with ATSSS servers <%s> and app server <%s>...\n", argv[1], argv[2]);

    // Initialize AuToSyndeSiS
    ret = autosyndesis_init(&ue_cb_ctx.atsss_state, 2);
    if (ret != 0) {
        fprintf(stderr, "Could not initialize AuToSyndeSiS. ReturnCode=%d\n", ret);
        return ret;
    }

    // Create the Femto-QUIC multipath client
    //ret = create_femto_mp_client(&mp_client, argv[1], ATSSS_UE_UPF_ALPN, ATSSS_UE_UPF_SNI, &mp_client_callback, &ue_cb_ctx);
    ret = create_femto_mp_client_bind_if(&mp_client, argv[1], ATSSS_UE_UPF_ALPN, ATSSS_UE_UPF_SNI, &mp_client_callback, &ue_cb_ctx, argv[3]);
    if (ret != 0) {
        fprintf(stderr, "Could not create Femto-QUIC multipath client. ReturnCode=%d\n", ret);
        return ret;
    }
    for (int i = 0; i < mp_client.num_paths && i < MAX_PATHS; i++) {
        g_path_available[i] = 1;
    }

    // set global pointer for interface monitor callbacks
    g_ue_cb_ctx = &ue_cb_ctx;
    
    // parse server IP and port from argv[1] for dynamic path addition
    // format: "ip1:port1,ip2:port2,..." - we use the first server as default
    {
        char server_list_copy[256];
        strncpy(server_list_copy, argv[1], sizeof(server_list_copy) - 1);
        server_list_copy[sizeof(server_list_copy) - 1] = '\0';
        
        char *first_server = strtok(server_list_copy, ",");
        if (first_server != NULL) {
            char *colon = strchr(first_server, ':');
            if (colon != NULL) {
                *colon = '\0';
                strncpy(g_server_ip, first_server, sizeof(g_server_ip) - 1);
                g_server_port = atoi(colon + 1);
                DEBUG_PRINT("[Dynamic Interface] Using server %s:%d for dynamic paths\n", 
                       g_server_ip, g_server_port);
            }
        }
    }
    
    // start automatic interface monitoring (with IP address change detection)
    DEBUG_PRINT("[Dynamic Interface] Starting automatic interface monitor...\n");
    ret = femto_init_interface_monitor_full(&interface_monitor, &ue_cb_ctx, 
                                             on_interface_up_callback, 
                                             on_interface_down_callback,
                                             on_addr_change_callback);
    if (ret == 0) {
        ret = femto_start_interface_monitor(&interface_monitor);
        if (ret == 0) {
            DEBUG_PRINT("[Dynamic Interface] Interface monitor started - will auto-detect interface and IP changes\n");
        } else {
            DEBUG_PRINT("[Dynamic Interface] Warning: Could not start interface monitor thread\n");
        }
    } else {
        DEBUG_PRINT("[Dynamic Interface] Warning: Could not initialize interface monitor\n");
    }


    // int connection_id = 0;  // TODO
    ue_cb_ctx.connection_id = 0;  // Will be updated when ATSSS_RESP_NEW_CONN is received

    // Open a new connection over ATSSS (use path 0) - requires path 0 to be up when starting
    atsss_new_connection(&ue_cb_ctx, 0, argv[2], strlen(argv[2])+1);

    //open fifo pipes
    int fd = open(argv[10], O_RDONLY | O_NONBLOCK);
    //ue_cb_ctx.outputfile = open(argv[9], O_WRONLY);

    // Set AuToSyndeSiS initial mode
    if (argc == 3) {
        // Use Active-Standby mode with default link 0 if no mode is specified
        ue_cb_ctx.atsss_actv_stdby_props.default_link = 0;
        autosyndesis_set_mode(&ue_cb_ctx.atsss_state, MODE_ACTIVE_STANDBY, &ue_cb_ctx.atsss_actv_stdby_props);
    } else {
        // Set the specified mode
        uint8_t atsss_mode = atoi(argv[4]);
        switch (atsss_mode) {
        case 0:  // Active-Standby
            ue_cb_ctx.atsss_actv_stdby_props.default_link = atoi(argv[5]);
            autosyndesis_set_mode(&ue_cb_ctx.atsss_state, MODE_ACTIVE_STANDBY, &ue_cb_ctx.atsss_actv_stdby_props);
            DEBUG_PRINT("Initial AuToSyndeSiS mode: Active-Standby (default_link=%d)\n", ue_cb_ctx.atsss_actv_stdby_props.default_link);
            break;
        case 1:  // Smallest Delay
            ue_cb_ctx.atsss_minrtt_props.switch_margin_us = atoi(argv[5]);
            autosyndesis_set_mode(&ue_cb_ctx.atsss_state, MODE_SMALLEST_DELAY, &ue_cb_ctx.atsss_minrtt_props);
            DEBUG_PRINT("Initial AuToSyndeSiS mode: Smallest Delay / MinRTT (margin=%luus)\n", ue_cb_ctx.atsss_minrtt_props.switch_margin_us);
            break;
        case 2:  // Load Balancing Round Robin
            // split parameter "1,2,3" into array {1, 2, 3}
            char *token = strtok(argv[5], ",");
            int num_paths = 0;
            while (token != NULL) {
                ue_cb_ctx.atsss_lb_rr_props.share[num_paths] = atoi(token);
                token = strtok(NULL, ",");
                num_paths++;
                if (num_paths >= MAX_PATHS || num_paths >= ue_cb_ctx.atsss_state.num_paths) {
                    DEBUG_PRINT("LB-RR parameters: num_paths exceeds MAX_PATHS=%d or num_paths=%d, num_paths is %d\n",
                           MAX_PATHS, ue_cb_ctx.atsss_state.num_paths, num_paths);
                    break;
                }
            }
            autosyndesis_set_mode(&ue_cb_ctx.atsss_state, MODE_LOAD_BALANCING, &ue_cb_ctx.atsss_lb_rr_props);
            DEBUG_PRINT("Initial AuToSyndeSiS mode: Load Balancing Round Robin with shares: { ");
            for (int i = 0; i < num_paths; i++) {
                DEBUG_PRINT("%d ", ue_cb_ctx.atsss_lb_rr_props.share[i]);
            }
            DEBUG_PRINT("}\n");
            break;
        case 3:  // Multipath Duplication
            ue_cb_ctx.atsss_minrtt_props.switch_margin_us = atoi(argv[5]);
            autosyndesis_set_mode(&ue_cb_ctx.atsss_state, MODE_MP_DUPLICATION, &ue_cb_ctx.atsss_minrtt_props);
            DEBUG_PRINT("Initial AuToSyndeSiS mode: Duplication / Opport. Redundant. (margin=%luus)\n", ue_cb_ctx.atsss_minrtt_props.switch_margin_us);
	    g_disable_apf = 1;
            break;
        default:
            DEBUG_PRINT("Invalid ATSSS mode: %d\n", atsss_mode);
            return 1;
        }

        num_iters = atoi(argv[6]);

        ue_cb_ctx.perf_cli_ctx.rtts = (uint64_t *)malloc(sizeof(uint64_t) * num_iters);
        ue_cb_ctx.perf_cli_ctx.arrival_times = (uint64_t *)malloc(sizeof(uint64_t) * num_iters);

        ue_cb_ctx.gobackn_enabled = atoi(argv[7]);

        sending_interval_us = atoi(argv[8]);
    }
    int request_rate = 0;
    if(getenv("RL_REQUEST_RATE")){
	request_rate = atoi(getenv("RL_REQUEST_RATE"));
    }
    if (request_rate == 0){
        printf("Request rate is 0 or the env variable wasn't retrieved\n");
        request_rate = 5000000;
    }

    int read_size = 65443;
    if(getenv("FIFO_READ_SIZE")){
        read_size = atoi(getenv("FIFO_READ_SIZE"));
    }

    init_curl_client();

    ue_cb_ctx.perf_cli_ctx.log_struct = malloc(sizeof(mpeval_log_entry_t) * num_iters);

    usleep(1000000);  // ensure that the ATSSS connection is established

    uint8_t send_buffer[ATSSS_INIT_PATH_HEADER_LEN];
    send_buffer[0] = ATSSS_PACKET_MAGIC;
    send_buffer[1] = ATSSS_CMD_INIT_PATH;
    uint64_t send_time = picoquic_current_time();
    send_buffer[3] = send_time >> 56; send_buffer[4] = send_time >> 48;
    send_buffer[5] = send_time >> 40; send_buffer[6] = send_time >> 32;
    send_buffer[7] = send_time >> 24; send_buffer[8] = send_time >> 16;
    send_buffer[9] = send_time >> 8;  send_buffer[10] = send_time;
    for (int p = 0; p < ue_cb_ctx.atsss_state.num_paths; p++) {
        // send_buffer[2] = connection_id;
        send_buffer[2] = ue_cb_ctx.connection_id;
        send_data_mp(&mp_client, (char *)send_buffer, ATSSS_INIT_PATH_HEADER_LEN, p);
        ue_cb_ctx.ping_times[p] = send_time;
        DEBUG_PRINT("Sent ATSSS_CMD_INIT_PATH to path %d\n", p);
    }
    usleep(500000);  // wait 500ms for accurate RTT measurements

    memcpy(ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload, "Hello World - This is padding to 65536 bytes.", 40);
    ue_cb_ctx.gobackn_time[ue_cb_ctx.connection_id] = time(NULL);



    uint32_t seq_num = 0;
    uint64_t last_ping = 0;
    uint64_t last_apf = 0;
    while (1) {
        //if (seq_num < num_iters) {
        // seq_num = ue_cb_ctx.sender_nextseqnum[connection_id];
        seq_num = ue_cb_ctx.sender_nextseqnum[ue_cb_ctx.connection_id];

        // TODO: Adjust for Selective-Repeat:
        // if (seq_num < num_iters && (seq_num < ue_cb_ctx.send_base[connection_id] + 3 || ue_cb_ctx.gobackn_enabled != 1)) {
        if (seq_num < num_iters && (seq_num < ue_cb_ctx.send_base[ue_cb_ctx.connection_id] + 3 || ue_cb_ctx.gobackn_enabled != 1)) {
            // Performance Client
            ue_cb_ctx.perf_cli_ctx.pkg_for_send.seq_num = seq_num;
            //ue_cb_ctx.perf_cli_ctx.pkg_for_send.timestamp_client = picoquic_current_time();
            ue_cb_ctx.perf_cli_ctx.pkg_for_send.timestamp_server = 0;
            ue_cb_ctx.perf_cli_ctx.pkg_for_send.path_cts = UINT8_MAX;
            ue_cb_ctx.perf_cli_ctx.pkg_for_send.path_stc = UINT8_MAX;

            if (fd < 0) {
                fd = open(argv[10], O_RDONLY | O_NONBLOCK);
            }

            ssize_t fifo_read_size = 0;

            if (fd >= 0) {
                fifo_read_size = read(fd, ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload + 45, read_size);
                if (fifo_read_size == 0) {
                    // Writer closed. Drop fd so we try to reopen next iteration.
                    close(fd);
                    fd = -1;
                } else if (fifo_read_size < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        fifo_read_size = 0; // no data yet, writer still connected
                    } else {
                        close(fd);
                        fd = -1;
                        fifo_read_size = 0;
                    }
                }
            }
            // if fd < 0 here (reopen failed too), fifo_read_size stays 0

            if (fifo_read_size > 0) {
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[39] = '\0';
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[40] = ATSSS_WEBRTC_DATA;
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[41] = seq_num;
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[42] = num_iters;
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[43] = fifo_read_size >> 8;
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[44] = fifo_read_size;
            } else {
                memset(ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload + 45, 0, 100);
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[40] = ATSSS_WEBRTC_DATA_EMPTY;
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[41] = seq_num;
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[42] = num_iters;
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[43] = 0;
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.payload[44] = 0;
            }

            // size of the packet to be sent
            size_t tx_len = offsetof(eval_perf_pkg_t, payload) + 45 + (size_t)fifo_read_size;

            max_stream_count_limit_reached = 0;
            for (int p = 0; p < ue_cb_ctx.atsss_state.num_paths; p++) {
                femto_client_t *client = &ue_cb_ctx.mp_client->clients[p];
                if (client == NULL || client->client_ctx == NULL) {
                    DEBUG_PRINT("DEBUG: Client/ClientCtx %d is NULL\n", p);
                    continue;
                }
                if (client->client_ctx->active_streams > MAX_ACTIVE_STREAMS) {
                    max_stream_count_limit_reached = 1;
                    break;
                }
            }
	
            // maximum packets in flight
            if (seq_num < ue_cb_ctx.highest_seqnum_acked + MAX_PKGS_IN_FLIGHT && !max_stream_count_limit_reached && fifo_read_size > 0) {
		        fifo_read_size = 0;
                usleep(sending_interval_us);
                ue_cb_ctx.perf_cli_ctx.pkg_for_send.timestamp_client = picoquic_current_time();

                /* ATSSS Mode: multipath duplication */
                if (seq_num == 0) {
                    DEBUG_PRINT("[DEBUG] Mode check: current_mode=%d, MODE_MP_DUPLICATION=%d, match=%d\n", 
                           ue_cb_ctx.atsss_state.current_mode, MODE_MP_DUPLICATION,
                           (ue_cb_ctx.atsss_state.current_mode == MODE_MP_DUPLICATION));
                }
                if (ue_cb_ctx.atsss_state.current_mode == MODE_MP_DUPLICATION) {
                    // Opportunistic Redundant: Send over all paths if normal transmission
                    int paths_used[MAX_PATHS];
                    int paths_used_count = 0;
                    for (int i = 0; i < ue_cb_ctx.atsss_state.num_paths; i++) {
                        int path_idx = choose_available_path(i, ue_cb_ctx.mp_client->num_paths);
                        ue_cb_ctx.perf_cli_ctx.pkg_for_send.path_cts = path_idx;
                        // (void)atsss_send_data(&mp_client, path_idx, connection_id, seq_num, (char *)&ue_cb_ctx.perf_cli_ctx.pkg_for_send, sizeof(eval_perf_pkg_t));
                        (void)atsss_send_data(&mp_client, path_idx, ue_cb_ctx.connection_id, seq_num, (char *)&ue_cb_ctx.perf_cli_ctx.pkg_for_send, tx_len);
                        if (paths_used_count < MAX_PATHS) {
                            paths_used[paths_used_count++] = path_idx;
                        }
                    }
                    next_path = (paths_used_count > 0) ? paths_used[0] : 0;
                    // log duplication immediately after sending
                    DEBUG_PRINT("Sent message over all paths (duplication mode): <%d> (paths: ", ue_cb_ctx.perf_cli_ctx.pkg_for_send.seq_num);
                    for (int i = 0; i < paths_used_count; i++) {
                        DEBUG_PRINT("%d ", paths_used[i]);
                    }
                    DEBUG_PRINT(")\n");
                    DEBUG_PRINT("DEMO01: UE --[all paths]--> UPF: Data %d\n", ue_cb_ctx.perf_cli_ctx.pkg_for_send.seq_num);
                }
                /* ATSSS Mode: others */
                else {
                    next_path = autosyndesis_determine_path(&ue_cb_ctx.atsss_state);
                    next_path = choose_available_path(next_path, ue_cb_ctx.mp_client->num_paths);
                    ue_cb_ctx.perf_cli_ctx.pkg_for_send.path_cts = next_path;
                    //int rc = atsss_send_data(&mp_client, next_path, connection_id, seq_num,
                    int rc = atsss_send_data(&mp_client, next_path, ue_cb_ctx.connection_id, seq_num,
                                             (char *)&ue_cb_ctx.perf_cli_ctx.pkg_for_send, tx_len);
                    /* best-effort failover: if send fails, retry once on another available path */
                    if (rc != 0) {
                        int fallback = choose_available_path(next_path + 1, ue_cb_ctx.mp_client->num_paths);
                        if (fallback != next_path) {
                            ue_cb_ctx.perf_cli_ctx.pkg_for_send.path_cts = fallback;
                            //(void)atsss_send_data(&mp_client, fallback, connection_id, seq_num,
                            (void)atsss_send_data(&mp_client, fallback, ue_cb_ctx.connection_id, seq_num,
                                                  (char *)&ue_cb_ctx.perf_cli_ctx.pkg_for_send, tx_len);
                            next_path = fallback;
                        }
                    }
                }
                // ue_cb_ctx.sender_nextseqnum[connection_id]++;
                ue_cb_ctx.sender_nextseqnum[ue_cb_ctx.connection_id]++;

                // store data in Go-Back-N buffer in case it is lost
                // ue_cb_ctx.gobackn_buff_ptr[connection_id][seq_num % GOBACKN_BUFF_SIZE] = (uint8_t*)malloc(sizeof(eval_perf_pkg_t));
                if (ue_cb_ctx.gobackn_enabled) {
                    int slot = seq_num % GOBACKN_BUFF_SIZE;
                    if (ue_cb_ctx.gobackn_buff_ptr[ue_cb_ctx.connection_id][slot] != NULL) {
                        free(ue_cb_ctx.gobackn_buff_ptr[ue_cb_ctx.connection_id][slot]);
                    }
                    ue_cb_ctx.gobackn_buff_ptr[ue_cb_ctx.connection_id][slot] = (uint8_t*)malloc(sizeof(eval_perf_pkg_t));
                    memcpy(ue_cb_ctx.gobackn_buff_ptr[ue_cb_ctx.connection_id][slot], &ue_cb_ctx.perf_cli_ctx.pkg_for_send, sizeof(eval_perf_pkg_t));
                    ue_cb_ctx.gobackn_buff_size[ue_cb_ctx.connection_id][slot] = sizeof(eval_perf_pkg_t);
                }

                // Selective Repeat: Store the time when the packet was sent
                //ue_cb_ctx.selrept_times[connection_id][seq_num % GOBACKN_BUFF_SIZE] = time(NULL);
                ue_cb_ctx.selrept_times[ue_cb_ctx.connection_id][seq_num % GOBACKN_BUFF_SIZE] = time(NULL);

                // log for non-duplication modes only (duplication already logged above)
                if (ue_cb_ctx.atsss_state.current_mode != MODE_MP_DUPLICATION) {
                    DEBUG_PRINT("Sent message over path %d: <%d>\n", next_path, ue_cb_ctx.perf_cli_ctx.pkg_for_send.seq_num);
                    //printf("DEMO01: Seq %lu: Sent DATA to UPF over PATH %d\n", (uint64_t)ue_cb_ctx.perf_cli_ctx.pkg_for_send.seq_num, next_path);
                    DEBUG_PRINT("DEMO01: UE ----[path %d]---> UPF: Data %d\n", next_path, ue_cb_ctx.perf_cli_ctx.pkg_for_send.seq_num);
                }
                fflush(stdout);
            } else {
                // Don't set it to high, otherwise throughput will be throttled.
                // The idea is to only sleep "long" if we actually sent something.
                // It might make sense to implement some smarter logic than this.
                // Do we actually need this delay? It just prevents 100% CPU usage.
                usleep(10);
            }

        }

        // send ping periodically to determine link status (rtt and packet loss)
        if (picoquic_current_time() - last_ping > g_ping_interval_us) {
            /* ping over all physical paths, not just scheduler paths, so a reactivated
             * path can be confirmed by pong even while the scheduler is temporarily using fewer paths. */
            for (int p = 0; p < ue_cb_ctx.mp_client->num_paths; p++) {
                if (p < MAX_PATHS && g_reconnecting[p]) {
                    continue;
                }
                if (ue_cb_ctx.ping_times[p] > 0) continue;
                send_time = picoquic_current_time();
                /* important: always set a valid ATSSS header.
                 * without setting con_id here, UPF may drop PING as "Invalid connection" and never answer PONG,
                 * which prevents path re-enable completion. */
                send_buffer[0] = ATSSS_PACKET_MAGIC;
                send_buffer[1] = ATSSS_CMD_PING;
                send_buffer[2] = ue_cb_ctx.connection_id;
                send_buffer[3] = send_time >> 56; send_buffer[4] = send_time >> 48;
                send_buffer[5] = send_time >> 40; send_buffer[6] = send_time >> 32;
                send_buffer[7] = send_time >> 24; send_buffer[8] = send_time >> 16;
                send_buffer[9] = send_time >> 8;  send_buffer[10] = send_time;
                {
                    int rc = send_data_mp(&mp_client, send_buffer, ATSSS_INIT_PATH_HEADER_LEN, p);
                    if (rc == 0) {
                        ue_cb_ctx.ping_times[p] = send_time;
                    } else if (ATSSS_DEBUG_DYNAMIC) {
                        DEBUG_PRINT("[debug][dyn] ping send failed: path_id=%d, rc=%d\n", p, rc);
                    }
                }

                if (ATSSS_DEBUG_DYNAMIC && p < MAX_PATHS && !g_path_available[p] && !g_probe_log_suppressed[p]) {
                    DEBUG_PRINT("[debug][dyn] probing ping sent on unavailable path %d (scheduler_num_paths=%d)\n",
                           p, ue_cb_ctx.atsss_state.num_paths);
                    g_probe_log_suppressed[p] = 1;
                }
            }
            last_ping = picoquic_current_time();
        }

        /* perform requested path reconnects (multi-cnx mp_client) */
        static uint64_t last_reconnect_check = 0;
        if (picoquic_current_time() - last_reconnect_check > 200000) { /* every 200ms */
            last_reconnect_check = picoquic_current_time();
            for (int p = 0; p < mp_client.num_paths && p < MAX_PATHS; p++) {
                if (!g_reconnect_requested[p] || g_reconnecting[p]) {
                    continue;
                }
                /* only reconnect when interface is up and has IPv4 */
                if (!femto_check_interface_available(mp_client.server_ifs[p])) {
                    continue;
                }
                struct sockaddr_storage tmp;
                if (femto_get_interface_ipv4(mp_client.server_ifs[p], &tmp) != 0) {
                    continue;
                }

                g_reconnecting[p] = 1;
                g_path_available[p] = 0;
                ue_cb_ctx.ping_times[p] = 0;
                DEBUG_PRINT("[AUTO] Reconnecting path %d (server=%s:%d, if_index=%d)\n",
                       p, mp_client.server_names[p], mp_client.server_ports[p], mp_client.server_ifs[p]);
                int rc = recreate_mp_client_path(&mp_client, p, &ue_cb_ctx);
                if (rc == 0) {
                    DEBUG_PRINT("[AUTO] Path %d client recreated; waiting for PONG to re-enable\n", p);
                    g_reconnect_requested[p] = 0;
                } else {
                    DEBUG_PRINT("[AUTO] Failed to recreate path %d client (rc=%d)\n", p, rc);
                }
                g_reconnecting[p] = 0;
            }
        }

        if (picoquic_current_time() - last_apf > request_rate){
	    last_apf = picoquic_current_time();
            if (ue_cb_ctx.atsss_state.current_mode == MODE_LOAD_BALANCING) {
	        contact_aue(&ue_cb_ctx, ue_cb_ctx.atsss_state.num_paths);
            }

        // log current path status for monitoring (sum across all clients)
        {
            int total_active = total_active_paths_clamped(&mp_client);
            int total_dynamic = 0;
            for (int i = 0; i < mp_client.num_paths; i++) {
                if (mp_client.clients[i].client_ctx != NULL) {
                    total_dynamic += mp_client.clients[i].client_ctx->mp_dynamic_path_count;
                }
            }
            int available = count_available_paths(mp_client.num_paths);
            if (total_active > available) {
                total_active = available;
            }
            DEBUG_PRINT("[Path Status] Active: %d, Dynamic: %d, Available: %d\n", total_active, total_dynamic, available);
        }
        
        // retry pending path probes that may have failed due to interface flapping
        retry_pending_path_probes(&mp_client, 5);  // retry every 5 seconds
	}

        // check for pings with no response (debounced)
        for (int p = 0; p < ue_cb_ctx.mp_client->num_paths; p++) {
            if (ue_cb_ctx.ping_times[p] > 0 && picoquic_current_time() - ue_cb_ctx.ping_times[p] > g_ping_timeout_us) {
                uint64_t delay = picoquic_current_time() - ue_cb_ctx.ping_times[p];
                ue_cb_ctx.ping_times[p] = 0;

                if (p >= 0 && p < MAX_PATHS && g_path_available[p]) {
                    g_missed_pongs[p]++;
                    if (g_missed_pongs[p] < g_ping_miss_threshold) {
                        if (ATSSS_DEBUG_DYNAMIC) {
                            DEBUG_PRINT("[debug][dyn] missed pong on path %d (miss=%d/%d, delay=%luus)\n",
                                   p, g_missed_pongs[p], g_ping_miss_threshold, delay);
                        }
                        continue;
                    }
                    g_missed_pongs[p] = 0;

                    DEBUG_PRINT("DEMO01: Path %d down! (delay > %lu us)\n", p, delay);
                    if (p < ue_cb_ctx.atsss_state.num_paths) {
                        autosyndesis_update_link_properties(&ue_cb_ctx.atsss_state, p, LINK_DOWN_THRESHOLD);
                    }
                    g_path_available[p] = 0;
                    /* do not shrink scheduler path count during flaps.
                     * available paths are selected via g_path_available[] + choose_available_path(). */
                }
            }
        }

        if (ue_cb_ctx.gobackn_enabled == 1) {  // Go-Back-N
        // Wait for response - handle retransmissions if necessary
        //while (ue_cb_ctx.perf_cli_ctx.rtts[ue_cb_ctx.perf_cli_ctx.seq_num] == 0) {
            for (int i = 0; i < MAX_CONNECTIONS; i++) {
                // Handle Go-Back-N timeout
                if (ue_cb_ctx.gobackn_time[i] > 0) {
                    if (time(NULL) - ue_cb_ctx.gobackn_time[i] > GOBACKN_TIMEOUT_SEC) {
                        DEBUG_PRINT("Go-Back-N timeout\n");
                        DEBUG_PRINT(" - con_id = %d\n", i);
                        DEBUG_PRINT(" - send_base = %d\n", ue_cb_ctx.send_base[i]);
                        DEBUG_PRINT(" - sender_nextseqnum = %d\n", ue_cb_ctx.sender_nextseqnum[i]);
                        ue_cb_ctx.gobackn_time[i] = time(NULL);

                        for (int seq = ue_cb_ctx.send_base[i]; seq < ue_cb_ctx.sender_nextseqnum[i]; seq++) {
                            DEBUG_PRINT("DEMO01: - Retransmitting seq_num=%u (con_id=%d)\n", seq, i);

                            uint8_t* data = ue_cb_ctx.gobackn_buff_ptr[i][seq % GOBACKN_BUFF_SIZE];
                            size_t len = ue_cb_ctx.gobackn_buff_size[i][seq % GOBACKN_BUFF_SIZE];
                            if (data == NULL) {
                                DEBUG_PRINT("FATAL ERROR: No data to retransmit for seq=%d of con_id=%i. Exiting...\n", seq, i);
                                exit(1);
                            }

                            // Opportunistic Redundant: Use MinRTT if retransmission
                            next_path = autosyndesis_determine_path(&ue_cb_ctx.atsss_state);
                            (void)atsss_send_data(&mp_client, next_path, i, seq, data, len);
                        }
                    }
                }
            }
        //}
        } else if (ue_cb_ctx.gobackn_enabled == 2) {  // Selective Repeat
            for (int i = 0; i < MAX_CONNECTIONS; i++) {
                for (int seq = ue_cb_ctx.send_base[i]; seq < ue_cb_ctx.sender_nextseqnum[i]; seq++) {
                    if (ue_cb_ctx.selrept_times[i][seq % GOBACKN_BUFF_SIZE] > 0 &&
                        time(NULL) - ue_cb_ctx.selrept_times[i][seq % GOBACKN_BUFF_SIZE] > GOBACKN_TIMEOUT_SEC) {
                        DEBUG_PRINT("Selective Repeat timeout\n");
                        DEBUG_PRINT(" - con_id = %d\n", i);
                        DEBUG_PRINT(" - seq_num = %d\n", seq);
                        DEBUG_PRINT(" - send_base = %d\n", ue_cb_ctx.send_base[i]);
                        DEBUG_PRINT(" - sender_nextseqnum = %d\n", ue_cb_ctx.sender_nextseqnum[i]);
                        DEBUG_PRINT(" - selrept_times = %ld\n", ue_cb_ctx.selrept_times[i][seq % GOBACKN_BUFF_SIZE]);
                        DEBUG_PRINT(" - time = %ld\n", time(NULL));
                        DEBUG_PRINT(" - diff = %ld\n", time(NULL) - ue_cb_ctx.selrept_times[i][seq % GOBACKN_BUFF_SIZE]);

                        DEBUG_PRINT("DEMO01: - Retransmitting seq_num=%u (con_id=%d)\n", seq, i);

                        uint8_t* data = ue_cb_ctx.gobackn_buff_ptr[i][seq % GOBACKN_BUFF_SIZE];
                        size_t len = ue_cb_ctx.gobackn_buff_size[i][seq % GOBACKN_BUFF_SIZE];
                        if (data == NULL) {
                            DEBUG_PRINT("FATAL ERROR: No data to retransmit for seq=%d of con_id=%i. Exiting...\n", seq, i);
                            exit(1);
                        }

                        // Opportunistic Redundant: Use MinRTT if retransmission
                        next_path = autosyndesis_determine_path(&ue_cb_ctx.atsss_state);
                        (void)atsss_send_data(&mp_client, next_path, i, seq, data, len);

                        ue_cb_ctx.selrept_times[i][seq % GOBACKN_BUFF_SIZE] = time(NULL);
                    }
                }
            }
        }
        /* if num_iters==0, run indefinitely (control-plane only / dynamic interface testing). */
        //if (num_iters > 0 && ue_cb_ctx.sender_nextseqnum[connection_id] >= (uint32_t)num_iters) {
        if (num_iters > 0 && ue_cb_ctx.sender_nextseqnum[ue_cb_ctx.connection_id] >= (uint32_t)num_iters) {
            DEBUG_PRINT("All messages sent\n");
            break;
        }
    }

    usleep(1000000);  // wait for the last responses to arrive (1 second)

    // Close the ATSSS connection
    for (int i = 0; i < mp_client.num_paths; i++) {
        //atsss_close_connection(&mp_client, i, connection_id);
        // TODO: fix this as this is currently buggy (causes segfault on UPF)
    }

    free_curl_client();

    free(ue_cb_ctx.perf_cli_ctx.rtts);
    free(ue_cb_ctx.perf_cli_ctx.arrival_times);

    free(ue_cb_ctx.perf_cli_ctx.log_struct);

    DEBUG_PRINT("DEMO01: EXIT...\n");

    return 0;
}

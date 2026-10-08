/*
 * Smart-Multipath-Project
 * User Plane Functionality (UPF) and controller
 * Uses AuToSyndeSis, Femto-QUIC and PicoQUIC
 * 
 * Author: David Munstein (2025)
 */

#include <time.h>
#include <pthread.h>
#include "femto_mp.h"
#include "autosyndesis.h"
#include "autosyndesis_actv_stdby.h"
#include "autosyndesis_smallest_delay.h"
#include "autosyndesis_lb_rr.h"
#include "atsss_project.h"

#include "cJSON.h"
#include <curl/curl.h>
#include "curl_functions.h"

static int g_disable_apf = 0;

typedef struct st_upf_state_t {
    femto_mp_server_t mp_server;
    uint8_t active[MAX_CONNECTIONS];
    uint8_t initialized[MAX_CONNECTIONS];
    char addresses[MAX_CONNECTIONS][128];
    femto_client_t *server_clients[MAX_CONNECTIONS];
    picoquic_cnx_t *ue_cnxs[MAX_CONNECTIONS][MAX_SERVERS];
    void *client_user_ctxs[MAX_CONNECTIONS];  // upf_state_t*

    autosyndesis_state_t atsss_state;
    autosyndesis_actv_stdby_props_t atsss_actv_stdby_props;
    autosyndesis_minrtt_props_t atsss_minrtt_props;
    autosyndesis_lb_rr_props_t atsss_lb_rr_props;

    uint32_t sender_nextseqnum[MAX_CONNECTIONS];
    uint32_t send_base[MAX_CONNECTIONS];
    uint32_t receiver_nextseqnum[MAX_CONNECTIONS];
    uint32_t highest_accepted_seqnum[MAX_CONNECTIONS];  // used for deduplication

    time_t gobackn_time[MAX_CONNECTIONS];

    uint8_t *gobackn_buff_ptr[MAX_CONNECTIONS][GOBACKN_BUFF_SIZE];
    size_t gobackn_buff_size[MAX_CONNECTIONS][GOBACKN_BUFF_SIZE];
    uint8_t gobackn_enabled;

    // deduplication
    uint8_t seq_seen_list[MAX_CONNECTIONS][1000];

    uint64_t ping_times[MAX_CONNECTIONS];

    volatile int shutdown;
} upf_state_t;

typedef struct st_upf_client_ctx_t {
    int con_id;
    upf_state_t *upf_state;
} upf_client_ctx_t;

// TODO: struct that stores connection information, should be passed as user_ctx to the server callback (which then maps conn_id to cnx_ptr)



void contact_ane(upf_state_t *upf_state, uint8_t num_paths){
    double ratio_wifi = 20;
    double ratio_5g = 0;
    const char *ratio_url = getenv("ANE_RATIO_URL");
    if (!ratio_url || ratio_url[0] == '\0') {
        ratio_url = "http://127.0.0.1:8001/ratio";  // default (old behavior)
    }

    int ret = get_ratio(ratio_url, &ratio_5g);
    DEBUG_PRINT("5G:%f\n", ratio_5g);
    if(ret != 0){
        DEBUG_PRINT("GETTING RATIO DIDN'T WORK\n");
    } else {
	upf_state->atsss_lb_rr_props.change_share = 1;
        uint8_t shares[] = {ratio_5g, 100-ratio_5g};
        memcpy(upf_state->atsss_lb_rr_props.share, shares, num_paths);
        autosyndesis_set_mode(&upf_state->atsss_state, MODE_LOAD_BALANCING, &upf_state->atsss_lb_rr_props);
   }
        DEBUG_PRINT("[DEBUG] AuToSyndeSiS: Set Mode to Load-Balancing (Round Robin) with shares: { ");
        for (int i = 0; i < num_paths; i++) {
            DEBUG_PRINT("%d ", upf_state->atsss_lb_rr_props.share[i]);
        }
        DEBUG_PRINT("}\n");
}

// Send data to the UE. Uses AuToSyndeSiS to determine the path (or Backup path if desired path is not initialized)
// Returns the path used if the desired path was used, the path used + 128 if the backup path was used, or -1 if no path was used
// Path not initialized means that no data was received from the UE on that path and so the connection id is not known
//  It would be possible to create a new connection ID from FemtoQUIC in theory, but this is not implemented yet
int atsss_ue_send_data(upf_state_t *upf_state, int con_id, uint8_t *send_buffer, size_t send_size) {
    // Opportunistic Redundant: Use MinRTT if retransmission
    int next_path = autosyndesis_determine_path(&upf_state->atsss_state);

    // desired path is not initialized - use any other path
    if (upf_state->ue_cnxs[con_id][next_path] == NULL) {
        DEBUG_PRINT("Desired path %d is not initialized. Using any other path.\n", next_path);
        for (int p = 0; p < upf_state->mp_server.num_paths; p++)
        {
            if (upf_state->ue_cnxs[con_id][p] == NULL) {
                continue;
            }
            send_data_to_client_mp(&upf_state->mp_server,
                                    p, upf_state->ue_cnxs[con_id][p],
                                    send_buffer, send_size);
            return p + 128;
        }
    }
    // use the desired path
    else {
        send_data_to_client_mp(&upf_state->mp_server,
                                    next_path, upf_state->ue_cnxs[con_id][next_path],
                                    send_buffer, send_size);
        return next_path;
    }

    return -1;
}


// Client Callback - called when data is received from the Application Server via normal QUIC
void *client_callback(uint8_t *data, size_t len, void *callback_ctx)
{
    upf_client_ctx_t *upf_client_ctx = (upf_client_ctx_t*)callback_ctx;

    int con_id = upf_client_ctx->con_id;

    uint32_t seq_num = upf_client_ctx->upf_state->sender_nextseqnum[con_id];

    DEBUG_PRINT("client_callback: Received data from Server (con_id=%i)\n", con_id);

    size_t send_size = len + 9;
    uint8_t send_buffer[BUFFER_SIZE];

    send_buffer[0] = ATSSS_PACKET_MAGIC;
    send_buffer[1] = ATSSS_RESP_DATA;
    send_buffer[2] = con_id;
    send_buffer[3] = seq_num >> 24;
    send_buffer[4] = seq_num >> 16;
    send_buffer[5] = seq_num >> 8;
    send_buffer[6] = seq_num;
    send_buffer[7] = len >> 8;
    send_buffer[8] = len;
    memcpy(send_buffer + 9, data, len);

    int path_used = atsss_ue_send_data(upf_client_ctx->upf_state, con_id, send_buffer, send_size);
    DEBUG_PRINT("Sent data (con_id=%i, seq_num=%u) from Server to UE on path %d %s\n", con_id, seq_num, (path_used & 127), (path_used & 128) ? "(backup path)" : "");
    //printf("DEMO01: Seq %lu: Sent DATA to UE over PATH %d\n", (uint64_t)seq_num, (path_used & 127));
    DEBUG_PRINT("DEMO01: UPF ----[path %d]---> UE: Response %d\n", (path_used & 127), seq_num);
    upf_client_ctx->upf_state->sender_nextseqnum[con_id] += 1;

    int slot = seq_num % GOBACKN_BUFF_SIZE;
    if (upf_client_ctx->upf_state->gobackn_enabled) {
        if (upf_client_ctx->upf_state->gobackn_buff_ptr[con_id][slot] != NULL) {
            free(upf_client_ctx->upf_state->gobackn_buff_ptr[con_id][slot]);
        }
        upf_client_ctx->upf_state->gobackn_buff_ptr[con_id][slot] = (uint8_t*)malloc(len);
        memcpy(upf_client_ctx->upf_state->gobackn_buff_ptr[con_id][slot], data, len);
        upf_client_ctx->upf_state->gobackn_buff_size[con_id][slot] = len;
    }
}


int atsss_new_conn(upf_state_t *upf_state, char *addr)
{
    int con_id = -1;
    upf_client_ctx_t *upf_client_ctx = (upf_client_ctx_t*)malloc(sizeof(upf_client_ctx_t));
    memset(upf_client_ctx, 0, sizeof(upf_client_ctx_t));

    upf_client_ctx->upf_state = upf_state;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!upf_state->active[i]) {
            con_id = i;
            break;
        }
    }

    if (con_id == -1) {
        DEBUG_PRINT("No available connection slots\n");
        return -1;
    }

    upf_state->server_clients[con_id] = (femto_client_t *)malloc(sizeof(femto_client_t));

    strcpy(upf_state->addresses[con_id], addr);

    upf_client_ctx->con_id = con_id;
    upf_state->active[con_id] = 1;
    upf_state->initialized[con_id] = 0;
    upf_state->client_user_ctxs[con_id] = upf_client_ctx;

    return con_id;
}

void atsss_close_conn(upf_state_t *upf_state, int con_id)
{
    destroy_femto_client(upf_state->server_clients[con_id]);
    upf_state->active[con_id] = 0;
    upf_state->send_base[con_id] = 0;
    upf_state->sender_nextseqnum[con_id] = 0;
    upf_state->receiver_nextseqnum[con_id] = 0;
    upf_state->gobackn_time[con_id] = 0;
    free((upf_state_t *)upf_state->client_user_ctxs[con_id]);
    free(upf_state->server_clients[con_id]);
    upf_state->client_user_ctxs[con_id] = NULL;
    upf_state->server_clients[con_id] = NULL;
    upf_state->shutdown = 1;
    DEBUG_PRINT("shutdown received\n");
}

// send data to the server
void atsss_send_data(upf_state_t *upf_state, int con_id, char *data, size_t data_len)
{
    if (upf_state->active[con_id] == 0) {
        DEBUG_PRINT("Connection %d is not active. Cannot send data.\n", con_id);
        return;
    }
    DEBUG_PRINT("Sending data on con_id=%i to addr=%s of size %d \n", con_id, upf_state->addresses[con_id], (data[43] << 8 | data [44]));  // TODO remove as data could not be stirng!
    send_data(upf_state->server_clients[con_id], data, data_len, 0);
}


void *mp_server_status_callback(uint8_t event, void *stream_id, void *user_ctx)
{
    uint8_t fin_or_event = event - 60;
    femto_mp_user_ctx_t *mp_user_ctx = (femto_mp_user_ctx_t*)user_ctx;
    int path_id = mp_user_ctx->path_id;
    upf_state_t *upf_state = (upf_state_t*)mp_user_ctx->user_ctx;
    int connection_id = 0;  // TODO !!!

    if (fin_or_event == picoquic_callback_path_deleted || fin_or_event == picoquic_callback_path_suspended) {
        upf_state->ue_cnxs[connection_id][path_id] = NULL;
    }

    return NULL;
}


// MP Server Callback - called when data is received from the UE via Multipath QUIC
void *mp_server_callback(uint8_t *receive_buffer, size_t receive_len, void *cb_ctx, uint8_t *response_buffer, size_t *response_len, picoquic_cnx_t *cnx)
{
    uint64_t receive_time = picoquic_current_time();
    size_t payload_len;
    uint8_t temp_buffer[BUFFER_SIZE];
    uint8_t con_id, atsss_mode;
    uint32_t seq_num;
    int next_path;
    uint64_t timestamp_client, tcu;

    femto_mp_user_ctx_t *mp_user_ctx = (femto_mp_user_ctx_t*)cb_ctx;
    int path_id = mp_user_ctx->path_id;
    upf_state_t *upf_state = (upf_state_t*)mp_user_ctx->user_ctx;


    if (receive_len < 3) {
        DEBUG_PRINT("Invalid ATSSS packet received (len=%lu)\n", receive_len);
        return NULL;
    }
    if (receive_buffer[0] != ATSSS_PACKET_MAGIC) {
        DEBUG_PRINT("Invalid ATSSS packet magic received: 0x%02X\n", receive_buffer[0]);
        *response_len = 0;
        return NULL;
    }

    switch (receive_buffer[1])
    {
    /* ATSSS CMD: New connection */
    case ATSSS_CMD_NEW_CONN:
        if (receive_len < 3) {
            DEBUG_PRINT("Invalid ATSSS packet received (len=%lu) for new connection\n", receive_len);
            return NULL;
        }
        payload_len = receive_buffer[2];  // length of the address string
        if (receive_len < 3 + payload_len) {
            DEBUG_PRINT("Invalid ATSSS packet received (len=%lu) for new connection with payload (payload_len=%lu)\n", receive_len, payload_len);
            return NULL;
        }
        if (payload_len > BUFFER_SIZE) {
            DEBUG_PRINT("Payload too large for new connection: %lu\n", payload_len);
            return NULL;
        }
        if (receive_buffer[3 + payload_len - 1] != '\0') {
            DEBUG_PRINT("New connection address string not null terminated.\n");
            return NULL;
        }
        memcpy(temp_buffer, receive_buffer + 3, payload_len);

        // don't create a new UPF<->Server connection for every UE<->UPF path!
        con_id = atsss_new_conn(upf_state, temp_buffer);

        response_buffer[0] = ATSSS_PACKET_MAGIC;
        response_buffer[1] = ATSSS_RESP_NEW_CONN;
        response_buffer[2] = con_id;
        upf_state->ue_cnxs[con_id][path_id] = cnx;
        *response_len = 3;
        break;
    /* ATSSS CMD: Close existing connection */
    case ATSSS_CMD_CLOSE_CONN:
        if (receive_len < 3) {
            DEBUG_PRINT("Invalid ATSSS packet received (len=%lu) for close connection\n", receive_len);
            return NULL;
        }
        con_id = receive_buffer[2];
        atsss_close_conn(upf_state, con_id);
        *response_len = 0;  // no response to UE
        break;
    /* ATSSS CMD: Path Initialization message or PING */
    case ATSSS_CMD_INIT_PATH:
    case ATSSS_CMD_PING:
        if (receive_len < ATSSS_INIT_PATH_HEADER_LEN) {
            DEBUG_PRINT("Invalid ATSSS packet received (len=%lu) for init path\n", receive_len);
            return NULL;
        }
        con_id = receive_buffer[2];
        if (!upf_state->active[con_id]) {
            DEBUG_PRINT("Invalid connection: con_id=%i ; active=%i\n", con_id, upf_state->active[con_id]);
            break;
        }
        upf_state->ue_cnxs[con_id][path_id] = cnx;

        timestamp_client = ((uint64_t)receive_buffer[3] << 56) + ((uint64_t)receive_buffer[4] << 48) + ((uint64_t)receive_buffer[5] << 40) + ((uint64_t)receive_buffer[6] << 32) +
                                    ((uint64_t)receive_buffer[7] << 24) + ((uint64_t)receive_buffer[8] << 16) + ((uint64_t)receive_buffer[9] << 8) + (uint64_t)receive_buffer[10];

        tcu = receive_time - timestamp_client;  // TODO: this is one-way delay and requires synchronized clocks!
        if (timestamp_client > 0) {
            //autosyndesis_update_link_properties(&upf_state->atsss_state, path_id, tcu);
            //printf("Updated link properties for path %d via INIT_PATH/CMD_PING cmd: RTT=%luus\n", path_id, tcu);
        }

        receive_buffer[1] = ATSSS_CMD_PONG;
        receive_buffer[3] = receive_time >> 56; response_buffer[4] = receive_time >> 48;
        receive_buffer[5] = receive_time >> 40; response_buffer[6] = receive_time >> 32;
        receive_buffer[7] = receive_time >> 24; response_buffer[8] = receive_time >> 16;
        receive_buffer[9] = receive_time >> 8;  response_buffer[10] = receive_time;

        *response_len = receive_len;
        memcpy(response_buffer, receive_buffer, receive_len);
        break;
    /* ATSSS CMD: Pong*/
    case ATSSS_CMD_PONG:
        if (receive_len < 11) {
            DEBUG_PRINT("Invalid ATSSS packet received (len=%lu) for init path\n", receive_len);
            return NULL;
        }
        timestamp_client = ((uint64_t)receive_buffer[3] << 56) + ((uint64_t)receive_buffer[4] << 48) + ((uint64_t)receive_buffer[5] << 40) + ((uint64_t)receive_buffer[6] << 32) +
                                    ((uint64_t)receive_buffer[7] << 24) + ((uint64_t)receive_buffer[8] << 16) + ((uint64_t)receive_buffer[9] << 8) + (uint64_t)receive_buffer[10];
        //tcu = receive_time - timestamp_client;
        if (upf_state->ping_times[path_id] == 0) {
            DEBUG_PRINT("Received PONG without a corresponding PING on path %d\n", path_id);
            break;
        }
        tcu = receive_time - upf_state->ping_times[path_id];
        if (timestamp_client > 0) {
            autosyndesis_update_link_properties(&upf_state->atsss_state, path_id, tcu);
            //printf("Updated link properties for path %d via CMD_PONG cmd: RTT=%luus\n", path_id, tcu);
            upf_state->ping_times[path_id] = 0;
        }
        *response_len = 0;
        break;
    /* ATSSS CMD: Send data over existing connection */
    case ATSSS_CMD_SEND_DATA:
        if (receive_len < 9) {
            DEBUG_PRINT("Invalid ATSSS packet received (len=%lu) for send data\n", receive_len);
            return NULL;
        }
        seq_num = (receive_buffer[3] << 24) + (receive_buffer[4] << 16) + (receive_buffer[5] << 8) + receive_buffer[6];
        payload_len = (receive_buffer[7] << 8) + receive_buffer[8];
        if (receive_len < 9 + payload_len) {
            DEBUG_PRINT("Invalid ATSSS packet received (len=%lu) for send data with payload\n", receive_len);
            return NULL;
        }
        if (payload_len > BUFFER_SIZE) {
            DEBUG_PRINT("Payload too large for send data: %lu\n", payload_len);
            return NULL;
        }

        con_id = receive_buffer[2];
        //upf_state->ue_cnxs[con_id][path_id] = cnx;
        if (!upf_state->active[con_id] || !upf_state->initialized[con_id]) {
            DEBUG_PRINT("Invalid connection: con_id=%i ; active=%i ; init=%i\n", con_id, upf_state->active[con_id], upf_state->initialized[con_id]);
            break;
        }
        //memcpy(temp_buffer, receive_buffer + 9, payload_len);
        upf_state->ue_cnxs[con_id][path_id] = cnx;

        *response_len = 0;

        // Deduplicate data (if duplicated across multiple paths)
        // if (seq_num < upf_state->highest_accepted_seqnum[con_id]) {
        //     break;
        // }

        // Deduplication using simple list - TODO verify that this works as intended in all scenarios!!
        if (seq_num > 990) {
            // Reset the sequence number list if the numbers rolled over
            memset(upf_state->seq_seen_list[con_id], 0, 1000);
        }
        if (upf_state->seq_seen_list[con_id][seq_num % 1000] > upf_state->atsss_state.num_paths) {
            // Reset the list. It seems like the sequence numbers are repeated on purpose
            memset(upf_state->seq_seen_list[con_id], 0, 1000);
        } else if (upf_state->seq_seen_list[con_id][seq_num % 1000] == 0) {
            upf_state->seq_seen_list[con_id][seq_num % 1000] += 1;

            if (upf_state->gobackn_enabled == 1) {  // Go-Back-N
                // ACK highest sequence number with Go-Back-N logic
                if (seq_num == upf_state->receiver_nextseqnum[con_id]) {
                    upf_state->receiver_nextseqnum[con_id] += 1;
                    upf_state->highest_accepted_seqnum[con_id] = seq_num;
                    //atsss_send_data(upf_state, con_id, temp_buffer, payload_len);
                    atsss_send_data(upf_state, con_id, receive_buffer + 9, payload_len);
                } else {
                    seq_num = upf_state->receiver_nextseqnum[con_id] - 1;
                }
            } else if (upf_state->gobackn_enabled == 2) {  // Selective Repeat
                // ACK all sequence numbers with Selective Repeat logic
                if (upf_state->receiver_nextseqnum[con_id] <= seq_num) {
                    upf_state->receiver_nextseqnum[con_id] = seq_num + 1;
                    upf_state->highest_accepted_seqnum[con_id] = seq_num;
                }
                atsss_send_data(upf_state, con_id, receive_buffer + 9, payload_len);
            } else {  // No ARQ or deduplication
                //atsss_send_data(upf_state, con_id, temp_buffer, payload_len);
                atsss_send_data(upf_state, con_id, receive_buffer + 9, payload_len);
            }
        }

        // Send ACK to UE
        temp_buffer[0] = ATSSS_PACKET_MAGIC;
        temp_buffer[1] = ATSSS_CMD_DATA_ACK;
        temp_buffer[2] = con_id;
        temp_buffer[3] = seq_num >> 24;
        temp_buffer[4] = seq_num >> 16;
        temp_buffer[5] = seq_num >> 8;
        temp_buffer[6] = seq_num;
        payload_len = 7;

        /* ATSSS Mode: MP Duplication */
        if (upf_state->atsss_state.current_mode == MODE_MP_DUPLICATION) {
            // Opportunistic Redundant: Send over all paths if normal transmission
            for (int i = 0; i < upf_state->atsss_state.num_paths; i++) {
                if (upf_state->ue_cnxs[con_id][i] == NULL) {
                    continue;
                }
                send_data_to_client_mp(&upf_state->mp_server,
                                        i, upf_state->ue_cnxs[con_id][i],
                                        temp_buffer, payload_len);
            }
            DEBUG_PRINT("Sent data ACK from UPF to UE on ALL paths (con_id=%d, seq_num=%u)\n", con_id, seq_num);
            //printf("DEMO01: Seq %lu: Sent ACK to UE over ALL PATHS. Original DATA was on PATH %d\n", (uint64_t)seq_num, path_id);
            DEBUG_PRINT("DEMO01: UPF --[all paths]--> UE: ACK for Data %d\n", seq_num);
        }
        /* ATSSS Mode: others */
        else {
            int path_used = atsss_ue_send_data(upf_state, con_id, temp_buffer, payload_len);
            DEBUG_PRINT("Sent data ACK (con_id=%i, seq_num=%u) from UPF to UE on path %d %s. Data was originally received on path %d.\n",
                    con_id, seq_num, (path_used & 127), (path_used & 128) ? "(backup path)" : "", path_id);
            //printf("DEMO01: Seq %lu: Sent ACK to UE over PATH %d. Original DATA was on PATH %d%s\n", (uint64_t)seq_num, (path_used & 127), path_id, (path_used & 128) ? "(backup path)" : "");
            DEBUG_PRINT("DEMO01: UPF ----[path %d]---> UE: ACK for Data %d\n", (path_used & 127), seq_num);
        }
        fflush(stdout);

        // Update the link properties if the data packet contains the timestamp from the client
        // Future TODO: This is not RTT but only UE->UPF time
        if (receive_len == sizeof(eval_perf_pkg_t) + ATSSS_SEND_DATA_HEADER_LEN) {
            eval_perf_pkg_t *pkg_received = (eval_perf_pkg_t *)(temp_buffer);
            uint64_t tcu = receive_time - pkg_received->timestamp_client;  // TODO: this is one-way delay and requires synchronized clocks!
            if (pkg_received->timestamp_client > 0) {
                //autosyndesis_update_link_properties(&upf_state->atsss_state, path_id, tcu);
                //printf("Updated link properties for path %d via SEND_DATA cmd: RTT=%luus\n", path_id, tcu);
            }
        }

        break;
    /* ATSSS ACK */
    case ATSSS_CMD_DATA_ACK:
        con_id = receive_buffer[2];
        upf_state->ue_cnxs[con_id][path_id] = cnx;
        seq_num = (receive_buffer[3] << 24) + (receive_buffer[4] << 16) + (receive_buffer[5] << 8) + receive_buffer[6];

        if (upf_state->gobackn_enabled) {
            upf_state->send_base[con_id] = seq_num + 1;
            if (upf_state->send_base[con_id] == upf_state->sender_nextseqnum[con_id]) {
                upf_state->gobackn_time[con_id] = time(NULL);
            }

            // free Go-Back-N buffer entry as data was acknowledged
            if (upf_state->gobackn_buff_ptr[con_id][seq_num % GOBACKN_BUFF_SIZE] == NULL) {
                DEBUG_PRINT("Received an invalid data ACK from the UE on path %i (con_id=%d, seq_num=%u).\n", path_id, con_id, seq_num);
            } else {
                DEBUG_PRINT("Received data ACK from the UE on path %i (con_id=%d, seq_num=%u)\n", path_id, con_id, seq_num);
                free(upf_state->gobackn_buff_ptr[con_id][seq_num % GOBACKN_BUFF_SIZE]);
                upf_state->gobackn_buff_ptr[con_id][seq_num % GOBACKN_BUFF_SIZE] = NULL;
                upf_state->gobackn_buff_size[con_id][seq_num % GOBACKN_BUFF_SIZE] = 0;
            }
        } else {
            DEBUG_PRINT("Received data ACK from the UE on path %i (con_id=%d, seq_num=%u)\n", path_id, con_id, seq_num);
        }
        break;
    }
}

void atsss_ping_ue(upf_state_t *upf_state, int con_id)
{
    uint64_t send_time = picoquic_current_time();
    uint8_t send_buffer[32];
    size_t send_size = 11;

    send_buffer[0] = ATSSS_PACKET_MAGIC;
    send_buffer[1] = ATSSS_CMD_PING;
    send_buffer[2] = con_id;
    send_buffer[3] = send_time >> 56; send_buffer[4] = send_time >> 48;
    send_buffer[5] = send_time >> 40; send_buffer[6] = send_time >> 32;
    send_buffer[7] = send_time >> 24; send_buffer[8] = send_time >> 16;
    send_buffer[9] = send_time >> 8;  send_buffer[10] = send_time;

    // Send PING request over all available paths
    for (int i = 0; i < upf_state->mp_server.num_paths; i++)
    {
        if (upf_state->ping_times[i] > 0) {
            continue;
        }
        if (upf_state->ue_cnxs[con_id][i] == NULL) {
            continue;
        }
        send_data_to_client_mp(&upf_state->mp_server,
                                i, upf_state->ue_cnxs[con_id][i],
                                send_buffer, send_size);
        upf_state->ping_times[i] = send_time;
    }
}






int main(int argc, char **argv)
{
    int ret;
    int running = 1;
    int num_paths;
    int ports[MAX_SERVERS];
    upf_state_t upf_state = { 0 };


    if (argc != 4 && argc != 7) {
        fprintf(stderr, "ATSSS UPF Usage: %s <list of ports> <cert_file> <key_file> [<initial mode> <mode parameters> <gobackn enabled>]\n", argv[0]);
        return 1;
    }
    printf("ATSSS UPF: Starting with ports <%s>...\n", argv[1]);

    // Parse the list of ports from argv[1] into ports
    num_paths = parse_port_list(argv[1], ports, MAX_SERVERS);
    if (num_paths < 1) {
        fprintf(stderr, "Invalid port list\n");
        return -1;
    }

    // create the femto multipath server for the UE to connect to
    ret = create_femto_mp_server(&upf_state.mp_server, num_paths, ports, ATSSS_UE_UPF_ALPN, argv[2], argv[3], &mp_server_callback, &upf_state);
    if (ret != 0) {
        fprintf(stderr, "Could not create Femto-QUIC multipath server. ReturnCode=%d\n", ret);
        return ret;
    }

    // set up AuToSyndeSiS
    ret = autosyndesis_init(&upf_state.atsss_state, 2);
    if (ret != 0) {
        fprintf(stderr, "Could not initialize AuToSyndeSiS. ReturnCode=%d\n", ret);
        return ret;
    }

    // Set AuToSyndeSiS initial mode
    if (argc == 4) {
        // Use Active-Standby mode with default link 0 if no mode is specified
        upf_state.atsss_actv_stdby_props.default_link = 0;
        autosyndesis_set_mode(&upf_state.atsss_state, MODE_ACTIVE_STANDBY, &upf_state.atsss_actv_stdby_props);
    } else {
        // Set the specified mode
        uint8_t atsss_mode = atoi(argv[4]);
        switch (atsss_mode) {
        case 0:  // Active-Standby
            upf_state.atsss_actv_stdby_props.default_link = atoi(argv[5]);
            autosyndesis_set_mode(&upf_state.atsss_state, MODE_ACTIVE_STANDBY, &upf_state.atsss_actv_stdby_props);
            DEBUG_PRINT("Initial AuToSyndeSiS mode: Active-Standby (default_link=%d)\n", upf_state.atsss_actv_stdby_props.default_link);
            break;
        case 1:  // Smallest Delay
            upf_state.atsss_minrtt_props.switch_margin_us = atoi(argv[5]);
            autosyndesis_set_mode(&upf_state.atsss_state, MODE_SMALLEST_DELAY, &upf_state.atsss_minrtt_props);
            DEBUG_PRINT("Initial AuToSyndeSiS mode: Smallest Delay / MinRTT (margin=%luus)\n", upf_state.atsss_minrtt_props.switch_margin_us);
            break;
        case 2:  // Load Balancing Round Robin
            // split parameter "1,2,3" into array {1, 2, 3}
            char *token = strtok(argv[5], ",");
            int num_paths = 0;
            while (token != NULL) {
                upf_state.atsss_lb_rr_props.share[num_paths] = atoi(token);
                token = strtok(NULL, ",");
                num_paths++;
                if (num_paths > MAX_PATHS || num_paths > upf_state.atsss_state.num_paths) {
                    DEBUG_PRINT("LB-RR parameters: num_paths exceeds MAX_PATHS=%d or num_paths=%d\n",
                           MAX_PATHS, upf_state.atsss_state.num_paths);
                    break;
                }
            }
            autosyndesis_set_mode(&upf_state.atsss_state, MODE_LOAD_BALANCING, &upf_state.atsss_lb_rr_props);
            DEBUG_PRINT("Initial AuToSyndeSiS mode: Load Balancing Round Robin with shares: { ");
            for (int i = 0; i < num_paths; i++) {
                DEBUG_PRINT("%d ", upf_state.atsss_lb_rr_props.share[i]);
            }
            DEBUG_PRINT("}\n");
            break;
        case 3:  // Multipath Duplication
            upf_state.atsss_minrtt_props.switch_margin_us = atoi(argv[5]);
            autosyndesis_set_mode(&upf_state.atsss_state, MODE_MP_DUPLICATION, &upf_state.atsss_minrtt_props);
	    g_disable_apf = 1;
            DEBUG_PRINT("Initial AuToSyndeSiS mode: Duplication / Opport. Redundant. (margin=%luus)\n", upf_state.atsss_minrtt_props.switch_margin_us);
            break;
        default:
            DEBUG_PRINT("Invalid ATSSS mode: %d\n", atsss_mode);
            return 1;
        }

        upf_state.gobackn_enabled = atoi(argv[6]);
    }
    init_curl_client();
    upf_state.shutdown = 0;
    uint64_t last_ane = 0;

    // Main loop:
    // - Handle new connections
    // - Handle Go-Back-N timeout
    uint64_t last_ping[MAX_CONNECTIONS] = { 0 };

    int request_rate = 0;
    if(getenv("RL_REQUEST_RATE")){
        request_rate = atoi(getenv("RL_REQUEST_RATE"));
    }
    if (request_rate == 0){
         printf("Request rate is 0 or the env variable wasn't set\n");
         request_rate = 5000000;
    }

    while (running) {
            uint8_t atsss_mode = atoi(argv[4]);
            if (picoquic_current_time() - last_ane > request_rate){
                last_ane = picoquic_current_time();
                if(atsss_mode == MODE_LOAD_BALANCING) {
                    contact_ane(&upf_state, num_paths);
                }
            }

        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            uint64_t current_time = picoquic_current_time();
            // Handle new connections
            if (upf_state.active[i] && !upf_state.initialized[i]) {
                DEBUG_PRINT("New connection (con_id=%i): <%s>\n", i, upf_state.addresses[i]);
                create_femto_client(upf_state.server_clients[i], upf_state.addresses[i],
                                    ATSSS_UPF_SERVER_ALPN, ATSSS_UPF_SERVER_SNI,
                                    &client_callback, upf_state.client_user_ctxs[i]);
                upf_state.initialized[i] = 1;
            }

            if (current_time - last_ping[i] > 500000) {  // ping every 500ms
                atsss_ping_ue(&upf_state, i);
                last_ping[i] = current_time;
            }


            // check for pings with no response
            for (int p = 0; p < upf_state.atsss_state.num_paths; p++) {
                if (upf_state.ping_times[p] > 0 && picoquic_current_time() - upf_state.ping_times[p] > 1000000) {
                    autosyndesis_update_link_properties(&upf_state.atsss_state, p, LINK_DOWN_THRESHOLD);
                    //printf("DEMO01: Detected link down on path %d: No response from ping %lu us ago\n", p, picoquic_current_time() - upf_state.ping_times[p]);
                    DEBUG_PRINT("DEMO01: Path %d down! (delay > %lu us)\n", p, picoquic_current_time() - upf_state.ping_times[p]);
                    upf_state.ping_times[p] = 0;
                }
            }


            // Handle the Go-Back-N timeout
            if (upf_state.gobackn_time[i] > 0 && upf_state.gobackn_enabled) {
                if (time(NULL) - upf_state.gobackn_time[i] > GOBACKN_TIMEOUT_SEC) {
                    DEBUG_PRINT("Go-Back-N timeout for connection %i\n", i);
                    upf_state.gobackn_time[i] = time(NULL);

                    for (int seq = upf_state.send_base[i]; seq < upf_state.sender_nextseqnum[i]; seq++) {
                        DEBUG_PRINT("- Retransmitting seq_num=%u (con_id=%d)\n", seq, i);  // TODO

                        uint8_t* data = upf_state.gobackn_buff_ptr[i][seq % GOBACKN_BUFF_SIZE];
                        size_t len = upf_state.gobackn_buff_size[i][seq % GOBACKN_BUFF_SIZE];
                        if (data == NULL) {
                            DEBUG_PRINT("FATAL ERROR: No data to retransmit for seq=%d of con_id=%i. Exiting...\n", seq, i);
                            exit(1);
                        }

                        size_t send_size = len + 9;
                        uint8_t send_buffer[BUFFER_SIZE];

                        send_buffer[0] = ATSSS_PACKET_MAGIC;
                        send_buffer[1] = ATSSS_RESP_DATA;
                        send_buffer[2] = i;
                        send_buffer[3] = seq >> 24;
                        send_buffer[4] = seq >> 16;
                        send_buffer[5] = seq >> 8;
                        send_buffer[6] = seq;
                        send_buffer[7] = len >> 8;
                        send_buffer[8] = len;
                        memcpy(send_buffer + 9, data, len);

                        int used_path = atsss_ue_send_data(&upf_state, i, send_buffer, send_size);
                        if (used_path == -1) {  // no path available -> no retransmission sent
                            DEBUG_PRINT("    No path available for retransmission of seq=%d of con_id=%i. Exiting...\n", seq, i);
                            exit(1);
                        } else if (used_path >= 128) {  // retransmission sent over Backup path as desired path is not initialized
                            int p = used_path - 128;
                            DEBUG_PRINT("    Retransmitted data for seq=%d of con_id=%i on path=%i (using Backup path).\n", seq, i, p);
                        } else {  // retransmission sent over desired path, selected by AuToSyndeSiS
                            DEBUG_PRINT("    Retransmitted data for seq=%d of con_id=%i on path=%i (using desired path).\n", seq, i, used_path);
                        }

                    }
                }
            }
        }

        usleep(10000);
        if(upf_state.shutdown){
                break;
            }
    }
    free_curl_client();
    destroy_femto_mp_server(&upf_state.mp_server);
}

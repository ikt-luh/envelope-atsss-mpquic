/*
 * Smart-Multipath-Project
 * 
 * Author: David Munstein
 */

#include <fcntl.h>

#ifndef ATSSS_PROJECT_H
#define ATSSS_PROJECT_H

#define MAX_CONNECTIONS 8  // maximum numbers of UEs that can connect to the UPF

// Parameters for Go-Back-N
#define GOBACKN_TIMEOUT_SEC 0.25
#define GOBACKN_BUFF_SIZE 200


#define ATSSS_UE_UPF_ALPN "atsss_example"  // ALPN UE<->UPF
#define ATSSS_UE_UPF_SNI "test.example.com"  // UE SNI
#define ATSSS_UPF_SERVER_ALPN "app_example"  // ALPN UPF<->Server
#define ATSSS_UPF_SERVER_SNI "test.example.com"  // UPF SNI


/* === ATSSS HAND-SHAKE & PROTOCOL DEFINITIONS === */

#define ATSSS_PACKET_MAGIC 0xA7

// 0xA7 0x10 [ADDR LEN] [ADDR str \0]
#define ATSSS_CMD_NEW_CONN 0x10

// 0xA7 0x11 [CON_ID]
#define ATSSS_RESP_NEW_CONN 0x11

// 0xA7 0x20 [CON_ID]
#define ATSSS_CMD_CLOSE_CONN 0x20

// 0xA7 0x30 [CON_ID] [SEQ NUM]*4 [DATA LEN high] [DATA LEN lw] [DATA]
#define ATSSS_CMD_SEND_DATA 0x30
#define ATSSS_SEND_DATA_HEADER_LEN 9

// 0xA7 0x31 [CON_ID] [SEQ NUM]*4 [DATA LEN high] [DATA LEN low] [DATA]
#define ATSSS_RESP_DATA 0x31

// 0xA7 0x32 [CON ID] [SEQ NUM]*4
#define ATSSS_CMD_DATA_ACK 0x32

// 0xA7 0x40 [CON_ID] [MODE] [PARAMETERS]
// Active-Standby:     0x00  [DEFAULT PATH]
// Smallest Delay:     0x01  [MARGIN << 24] [MARGIN << 16] [MARGIN << 8] [MARGIN]
// Load Bal (RR):      0x02  [NUM PATHS] [SHARE 1] [SHARE 2] ... [SHARE N]
// MP Duplication:     0x03  [MARGIN << 24] [MARGIN << 16] [MARGIN << 8] [MARGIN]
#define ATSSS_CMD_SET_MODE 0x40
#define ATSSS_MODE_ACTV_STDBY 0x00
#define ATSSS_MODE_SMAL_DELAY 0x01
#define ATSSS_MODE_LB_RR 0x02
#define ATSSS_MODE_MP_DUP 0x03

// 0xA7 0x99 [CON_ID] [SEND TIMESTAMP]*8
#define ATSSS_CMD_INIT_PATH 0x99
#define ATSSS_INIT_PATH_HEADER_LEN 11

// 0xA7 0xA0 [CON_ID] [SEND TIMESTAMP]*8
#define ATSSS_CMD_PING 0xA0
// 0xA7 0xA1 [CON_ID] [SEND TIMESTAMP]*8
#define ATSSS_CMD_PONG 0xA1

#define ATSSS_WEBRTC_DATA 0xEA
#define ATSSS_WEBRTC_DATA_EMPTY 0xEB


/* evaluation client packet */
typedef struct {
    uint32_t seq_num;
    uint64_t timestamp_client;
    uint64_t timestamp_server;
    uint8_t path_cts;  // path client-to-server
    uint8_t path_stc;  // path server-to-client
    uint8_t payload[65488];  // ensure that sizeof(eval_perf_pkg_t) <= 65520 (femto_quic buffer size minus atsss header)
} eval_perf_pkg_t;

#endif

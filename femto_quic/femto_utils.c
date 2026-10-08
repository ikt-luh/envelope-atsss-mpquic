/*
 * Femto-QUIC, a simple wrapper library for PicoQUIC
 * Based on the PicoQUIC samples by Christian Huitema
 * 
 * Author: David Munstein (2025)
 */

 #include <string.h>
#include "femto.h"

/**
 * Parse the server list
 * Accepts a comma-separated list of "IP:Port" pairs, e.g. "10.0.1.2:4443,10.0.2.2:4444"
 * @param[in] server_string the server string to parse
 * @param[out] ip_addresses the array of IP addresses to fill
 * @param[out] ports the array of ports to fill
 * @param[out] interfaces the array of interfaces to fill
 * @param[in] max_servers the maximum number of servers to parse
 */
int parse_server_list(const char *server_string, char ip_addresses[][MAX_SERVER_NAME_LENGTH], int *ports, int max_servers)
{
    char input_copy[MAX_SERVERS * MAX_SERVER_NAME_LENGTH];
    strncpy(input_copy, server_string, sizeof(input_copy) - 1);
    input_copy[sizeof(input_copy) - 1] = '\0';
    char *token;
    char *saveptr1;
    int server_count = 0;

    // Tokenize by "," to get each "IP:Port" pair
    token = strtok_r(input_copy, ",", &saveptr1);
    while (token != NULL && server_count < max_servers) {
        char *saveptr2;
        char *ip = strtok_r(token, ":", &saveptr2);
        char *port_str = strtok_r(NULL, ":", &saveptr2);

        if (ip != NULL && port_str != NULL) {
            strncpy(ip_addresses[server_count], ip, 15);
            ip_addresses[server_count][15] = '\0';
            ports[server_count] = atoi(port_str);
            if (ports[server_count] == 0) {
                return -1;
            }
            server_count++;
        }

        token = strtok_r(NULL, ",", &saveptr1);
    }

return server_count;

}

/**
 * Parse the port list
 * Accepts a comma-separated list of ports, e.g. "4443,4444"
 * @param[in] port_string the port string to parse
 * @param[out] ports the array of ports to fill
 * @param[in] max_ports the maximum number of ports to parse
 */
int parse_port_list(const char *port_string, int *ports, int max_ports)
{
    char input_copy[MAX_SERVERS * 5];
    strncpy(input_copy, port_string, sizeof(input_copy) - 1);
    input_copy[sizeof(input_copy) - 1] = '\0';

    char *token;
    char *saveptr1;
    int port_count = 0;

    // Tokenize by "," to get each port
    token = strtok_r(input_copy, ",", &saveptr1);
    while (token != NULL && port_count < max_ports) {
        ports[port_count] = atoi(token);
        if (ports[port_count] == 0) {
            return -1;
        }
        port_count++;

        token = strtok_r(NULL, ",", &saveptr1);
    }

    return port_count;
}

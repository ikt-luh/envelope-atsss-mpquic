#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"
#include "femto.h"


static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
void parse_steering_mode_info(const char *json_str, double *wifi_ratio, double *fiveg_ratio);
int get_ratio(const char *apiRoot, double *ratio);
void init_curl_client();
void free_curl_client();

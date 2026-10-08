#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cJSON.h"
#include "curl_functions.h"

static CURL* p_curl = NULL;

void init_curl_client(void){
    p_curl = curl_easy_init();
    if(p_curl){
        DEBUG_PRINT("Curl initialized\n");
    }
}

void free_curl_client(void){
    if(p_curl){
        curl_easy_cleanup(p_curl);
        p_curl = NULL;
        DEBUG_PRINT("Curl cleaned up\n");
    }
}

struct MemoryStruct {
    char *memory;
    size_t size;
};


static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) return 0;

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

void parse_steering_mode_info(const char *json_str, double *wifi_ratio, double *fiveg_ratio) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        fprintf(stderr, "Error parsing JSON.\n");
        return;
    }
    
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi_ratio");
    cJSON *fiveg = cJSON_GetObjectItemCaseSensitive(root, "fiveg_ratio");
    
    if (cJSON_IsNumber(wifi) && cJSON_IsNumber(fiveg)) {
        DEBUG_PRINT("Ratios:\n");
        DEBUG_PRINT("  WiFi:  %.2f (%.1f%%)\n", wifi->valuedouble, wifi->valuedouble * 100);
        DEBUG_PRINT("  5G:    %.2f (%.1f%%)\n", fiveg->valuedouble, fiveg->valuedouble * 100);
        
        if (wifi_ratio) *wifi_ratio = wifi->valuedouble;
        if (fiveg_ratio) *fiveg_ratio = fiveg->valuedouble;
    } else {
        fprintf(stderr, "wifi_ratio or fiveg_ratio fields missing or invalid.\n");
    }
    
    cJSON_Delete(root);
}

int get_ratio(const char *apiRoot, double *ratio) {
    CURLcode res;
    struct MemoryStruct chunk = {0};
    
    if (!p_curl) {
        fprintf(stderr, "Curl isn't initialized\n");
        return 1;
    }
    
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    
    curl_easy_setopt(p_curl, CURLOPT_URL, apiRoot);
    curl_easy_setopt(p_curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(p_curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(p_curl, CURLOPT_WRITEDATA, (void *)&chunk);
    
    DEBUG_PRINT("Requesting ratio from: %s\n", apiRoot);
    
    res = curl_easy_perform(p_curl);
    
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        curl_slist_free_all(headers);
        free(chunk.memory);
        return 1;
    }
    cJSON *root = cJSON_Parse(chunk.memory);
    if (!root) {
        fprintf(stderr, "Error parsing JSON.\n");
        curl_slist_free_all(headers);
        free(chunk.memory);
        return 1;
    }
    
    cJSON *ratio_json = cJSON_GetObjectItemCaseSensitive(root, "ratio");
    if (cJSON_IsNumber(ratio_json)) {
        if (ratio) *ratio = ratio_json->valuedouble;
        DEBUG_PRINT("Retrieved ratio: %.2f (%.1f%%)\n", ratio_json->valuedouble, ratio_json->valuedouble * 100);
    } else {
        fprintf(stderr, "Ratio field missing or invalid.\n");
        cJSON_Delete(root);
        curl_slist_free_all(headers);
        free(chunk.memory);
        return 1;
    }
    
    cJSON_Delete(root);
    curl_slist_free_all(headers);
    free(chunk.memory);
    
    return 0;
}


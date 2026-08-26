#include "ConfigLoader.h"
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "Logger.h"

static const char* CONFIG_TAG = "CONFIG_LOADER";

// Dichiarazione dei simboli generati dal linker per il JSON
extern const uint8_t eg91_config_json_start[] asm("_binary_eg91_config_json_start");
extern const uint8_t eg91_config_json_end[]   asm("_binary_eg91_config_json_end");

bool load_eg91_config(Eg91Config& config) {
    // Il file è già in memoria, puntiamo direttamente all'inizio
    const char* json_data = (const char*)eg91_config_json_start;

    // cJSON_Parse legge direttamente dalla stringa in memoria
    cJSON* root = cJSON_Parse(json_data);

    if (root == NULL) {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            Logger::instance().error(CONFIG_TAG, "Integrated JSON parsing error near: %s", error_ptr);
        }
        return false;
    }

    cJSON* item = NULL;

    // ... (Mantieni tutta la logica di assegnazione campi esistente) ...
    item = cJSON_GetObjectItem(root, "apn");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) config.apn = item->valuestring;

    item = cJSON_GetObjectItem(root, "endpoint");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) config.endpoint = item->valuestring;

    item = cJSON_GetObjectItem(root, "port");
    if (cJSON_IsNumber(item)) config.port = item->valueint;

    item = cJSON_GetObjectItem(root, "client_id");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) config.client_id = item->valuestring;

    item = cJSON_GetObjectItem(root, "auth_type");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) config.auth_type = item->valuestring;

    item = cJSON_GetObjectItem(root, "mqtt_user");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) config.mqtt_user = item->valuestring;

    item = cJSON_GetObjectItem(root, "mqtt_pass");
    if (cJSON_IsString(item) && (item->valuestring != NULL)) config.mqtt_pass = item->valuestring;

    cJSON_Delete(root);

    Logger::instance().info(CONFIG_TAG, "Configuration successfully loaded (APN: %s)", config.apn.c_str());
    return true;
}
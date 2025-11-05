// This is the serial only version

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <esp_log.h>
#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <set>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "credentials.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#define TAG "merged"

SemaphoreHandle_t wifi_connected = NULL;

struct HttpData {
    std::string thoughts;
    std::string answer;
    std::string response_buffer;
    cJSON *grounding_metadata = nullptr;
};

void print_citations(cJSON* metadata) {
    cJSON *supports = cJSON_GetObjectItem(metadata, "groundingSupports");
    cJSON *chunks = cJSON_GetObjectItem(metadata, "groundingChunks");
    if (!supports || !chunks || !cJSON_IsArray(supports) || !cJSON_IsArray(chunks)) {
        return;
    }

    int chunk_size = cJSON_GetArraySize(chunks);
    std::set<int> used_indices;
    for (int i = 0; i < cJSON_GetArraySize(supports); ++i) {
        cJSON *sup = cJSON_GetArrayItem(supports, i);
        cJSON *indices = cJSON_GetObjectItem(sup, "groundingChunkIndices");
        if (!indices || !cJSON_IsArray(indices)) continue;
        for (int k = 0; k < cJSON_GetArraySize(indices); ++k) {
            cJSON *i_json = cJSON_GetArrayItem(indices, k);
            if (!i_json || !cJSON_IsNumber(i_json)) continue;
            int idx = i_json->valueint;
            if (idx >= 0 && idx < chunk_size) {
                used_indices.insert(idx);
            }
        }
    }

    if (used_indices.empty()) return;

    printf("\nCitations:\n");
    for (int idx : used_indices) {
        cJSON *chunk = cJSON_GetArrayItem(chunks, idx);
        cJSON *web = cJSON_GetObjectItem(chunk, "web");
        cJSON *uri_json = cJSON_GetObjectItem(web, "uri");
        if (!chunk || !web || !uri_json || !cJSON_IsString(uri_json)) continue;
        cJSON *title_json = cJSON_GetObjectItem(web, "title");
        std::string title_str = (title_json && cJSON_IsString(title_json)) ?
            std::string(title_json->valuestring).substr(0, 255) : "";
        std::string prefix = title_str.empty() ? "" : title_str + ": ";
        std::string citation_line = "[" + std::to_string(idx + 1) + "] " + prefix +
            uri_json->valuestring + "\n";
        printf("%s", citation_line.c_str());
    }
}

void process_data_line(const std::string& line, HttpData* data) {
    if (line.rfind("data: ", 0) != 0) {
        return;
    }
    std::string json_str = line.substr(6);
    json_str.erase(0, json_str.find_first_not_of(" \t"));
    json_str.erase(json_str.find_last_not_of(" \t") + 1);
    if (json_str == "[DONE]") {
        return;
    }
    cJSON *json = cJSON_Parse(json_str.c_str());
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", json_str.c_str());
        return;
    }
    cJSON *candidates = cJSON_GetObjectItem(json, "candidates");
    if (candidates && cJSON_IsArray(candidates)) {
        cJSON *candidate = cJSON_GetArrayItem(candidates, 0);
        if (candidate) {
            cJSON *content = cJSON_GetObjectItem(candidate, "content");
            if (content) {
                cJSON *parts = cJSON_GetObjectItem(content, "parts");
                if (parts && cJSON_IsArray(parts)) {
                    cJSON *part = cJSON_GetArrayItem(parts, 0);
                    if (part) {
                        cJSON *thought_flag = cJSON_GetObjectItem(part, "thought");
                        cJSON *text = cJSON_GetObjectItem(part, "text");
                        if (text && cJSON_IsString(text)) {
                            bool is_thought = thought_flag && cJSON_IsBool(thought_flag) && cJSON_IsTrue(thought_flag);
                            std::string& target = is_thought ? data->thoughts : data->answer;
                            const char* header = is_thought ? "Thoughts :\n" : "Answer:\n";
                            if (target.empty()) {
                                printf("%s", header);
                            }
                            printf("%s", text->valuestring);
                            target += text->valuestring;
                        }
                    }
                }
            }
            cJSON *gmeta = cJSON_GetObjectItem(candidate, "groundingMetadata");
            if (gmeta && cJSON_IsObject(gmeta)) {
                if (data->grounding_metadata) {
                    cJSON_Delete(data->grounding_metadata);
                }
                data->grounding_metadata = cJSON_Duplicate(gmeta, 1);
            }
        }
    }
    cJSON_Delete(json);
}

esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    HttpData* data = (HttpData*)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                return ESP_OK;
            }
            data->response_buffer.append((char*)evt->data, evt->data_len);
            size_t pos;
            while ((pos = data->response_buffer.find('\n')) != std::string::npos) {
                std::string line = data->response_buffer.substr(0, pos);
                data->response_buffer.erase(0, pos + 1);
                process_data_line(line, data);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

void process_full_buffer(HttpData* data) {
    size_t pos;
    while ((pos = data->response_buffer.find('\n')) != std::string::npos) {
        std::string line = data->response_buffer.substr(0, pos);
        data->response_buffer.erase(0, pos + 1);
        process_data_line(line, data);
    }
    if (!data->response_buffer.empty()) {
        std::string line = data->response_buffer;
        data->response_buffer.clear();
        process_data_line(line, data);
    }
}

void http_task(void *pvParameters) {
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    if (xSemaphoreTake(wifi_connected, pdMS_TO_TICKS(60000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        char *line = linenoise("Enter prompt> ");
        if (!line || strlen(line) == 0) {
            if (line) free(line);
            continue;
        }

        HttpData data;
        data.response_buffer.reserve(1024);
        data.grounding_metadata = nullptr;

        cJSON *root = cJSON_CreateObject();
        cJSON *contents = cJSON_AddArrayToObject(root, "contents");
        cJSON *content = cJSON_CreateObject();
        cJSON_AddItemToArray(contents, content);
        cJSON *parts = cJSON_AddArrayToObject(content, "parts");
        cJSON *part = cJSON_CreateObject();
        cJSON_AddStringToObject(part, "text", line);
        cJSON_AddItemToArray(parts, part);

        cJSON *tools = cJSON_AddArrayToObject(root, "tools");
        cJSON *tool_obj = cJSON_CreateObject();
        cJSON_AddObjectToObject(tool_obj, "google_search");
        cJSON_AddItemToArray(tools, tool_obj);

        cJSON *generationConfig = cJSON_AddObjectToObject(root, "generationConfig");
        cJSON *thinkingConfig = cJSON_AddObjectToObject(generationConfig, "thinkingConfig");
        cJSON_AddBoolToObject(thinkingConfig, "includeThoughts", 1);
        char *post_data = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        char url[256];
        snprintf(url, sizeof(url), "https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-latest:streamGenerateContent?alt=sse&key=%s", API_KEY);

        esp_http_client_config_t config = {};
        config.url = url;
        config.method = HTTP_METHOD_POST;
        config.event_handler = http_event_handler;
        config.user_data = &data;
        config.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));

        ESP_LOGI(TAG, "Sending prompt: %s", line);
        esp_err_t err = esp_http_client_perform(client);
        process_full_buffer(&data);
        int status_code = -1;
        if (err == ESP_OK) {
            status_code = esp_http_client_get_status_code(client);
            printf("\n");
            if (data.grounding_metadata) {
                print_citations(data.grounding_metadata);
                cJSON_Delete(data.grounding_metadata);
            } else {
                printf("No grounding metadata available.\n");
            }
            ESP_LOGI(TAG, "Stream processing complete. Final thoughts: %zu chars, answer: %zu chars", data.thoughts.length(), data.answer.length());
            ESP_LOGI(TAG, "HTTP POST Status = %d", status_code);
        } else {
            ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
        free(post_data);
        linenoiseHistoryAdd(line);
        free(line);
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START || event_id == WIFI_EVENT_STA_DISCONNECTED) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected, got IP");
        if (wifi_connected != NULL) {
            xSemaphoreGive(wifi_connected);
        }
    }
}

static void init_console(void) {
    uart_config_t uart_config = {};
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_APB;

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
    esp_vfs_dev_uart_use_driver(UART_NUM_0);

    esp_console_config_t console_config = {};
    console_config.max_cmdline_length = 256;
    console_config.max_cmdline_args = 8;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    console_config.hint_color = atoi("36");
#endif

    ESP_ERROR_CHECK(esp_console_init(&console_config));

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(10);
    linenoiseAllowEmpty(false);
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_console();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASS);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_connected = xSemaphoreCreateBinary();
    
    xTaskCreate(http_task, "http_task", 20 * 1024, NULL, 5, NULL);

    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
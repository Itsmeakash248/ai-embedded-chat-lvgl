#include <LGFX.hpp>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <stdlib.h>
#include <esp_vfs_fat.h>
#include <wear_levelling.h>
#include <esp_log.h>
#include <cJSON.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
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

LGFX lcd;
wl_handle_t wl_handle = WL_INVALID_HANDLE;
SemaphoreHandle_t wifi_connected = NULL;

struct HttpData {
    std::string thoughts;
    std::string answer;
    std::string response_buffer;
    cJSON *grounding_metadata = nullptr;
};

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    lcd.startWrite();
    lcd.setAddrWindow(area->x1, area->y1, w, h);
    lcd.writePixelsDMA((uint16_t *)color_p, w * h, true);
    lcd.endWrite();
    lv_display_flush_ready(disp);
}

static uint32_t get_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
    int x = 0, y = 0;
    if (lcd.getTouch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static bool load_calibration_data(uint16_t caldata[8]) {
    if (wl_handle == WL_INVALID_HANDLE) {
        return false;
    }
    FILE *f = fopen("/storage/caldata.json", "r");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json_str = (char *)malloc(len + 1);
    if (!json_str) {
        fclose(f);
        return false;
    }
    size_t read_bytes = fread(json_str, 1, len, f);
    json_str[len] = '\0';
    fclose(f);
    if (read_bytes != (size_t)len) {
        free(json_str);
        return false;
    }
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root || !cJSON_IsArray(root) || cJSON_GetArraySize(root) != 8) {
        cJSON_Delete(root);
        return false;
    }
    bool success = true;
    for (int i = 0; i < 8; ++i) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!item || !cJSON_IsNumber(item)) {
            success = false;
            break;
        }
        caldata[i] = (uint16_t)cJSON_GetNumberValue(item);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Calibration data loaded from JSON file.");
    return success;
}

static bool save_calibration_data(const uint16_t caldata[8]) {
    if (wl_handle == WL_INVALID_HANDLE) {
        ESP_LOGW(TAG, "Cannot save calibration data: FATFS not mounted.");
        return false;
    }
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        cJSON_AddItemToArray(root, cJSON_CreateNumber(caldata[i]));
    }
    char *json_str = cJSON_Print(root);
    if (!json_str) {
        cJSON_Delete(root);
        return false;
    }
    FILE *f = fopen("/storage/caldata.json", "w");
    if (!f) {
        cJSON_free(json_str);
        cJSON_Delete(root);
        return false;
    }
    size_t json_len = strlen(json_str);
    size_t written_bytes = fwrite(json_str, 1, json_len, f);
    fclose(f);
    cJSON_free(json_str);
    cJSON_Delete(root);
    if (written_bytes != json_len) {
        ESP_LOGE(TAG, "Failed to write JSON to file.");
        return false;
    }
    ESP_LOGI(TAG, "Calibration data saved to JSON file.");
    return true;
}

static void perform_calibration(uint16_t caldata[8]) {
    ESP_LOGI(TAG, "Performing touch calibration...");
    lcd.calibrateTouch(caldata, lcd.color565(255, 0, 0), lcd.color565(0, 0, 0), 20);
}

#include <string.h>  // Assuming this is needed for strcmp; add if not present

lv_obj_t * home_list;
lv_obj_t * home_cont;
lv_obj_t * touch_ind;
lv_obj_t * status_bar;
lv_obj_t * settings_cont;

void cr_status_bar();
void cr_home_scr();
void cr_settings_scr();

static void fade_out_home_cb(lv_anim_t * a);
static void fade_out_settings_cb(lv_anim_t * a);

static void event_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target_obj(e);
    if (code == LV_EVENT_CLICKED) {
        const char * btn_text = lv_list_get_button_text(home_list, obj);
        LV_LOG_USER("Clicked: %s", btn_text);
        if (strcmp(btn_text, "Settings") == 0) {
            lv_obj_set_style_opa(home_cont, LV_OPA_COVER, 0);
            lv_anim_t a_out;
            lv_anim_init(&a_out);
            lv_anim_set_var(&a_out, home_cont);
            lv_anim_set_values(&a_out, LV_OPA_COVER, LV_OPA_TRANSP);
            lv_anim_set_time(&a_out, 300);
            lv_anim_set_exec_cb(&a_out, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
            lv_anim_set_ready_cb(&a_out, fade_out_home_cb);
            lv_anim_start(&a_out);
        }
        // Add similar if statements for other buttons if needed (e.g., for "Gemini AI", "Weather", "About")
    }
}

static void back_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_obj_set_style_opa(settings_cont, LV_OPA_COVER, 0);
        lv_anim_t a_out;
        lv_anim_init(&a_out);
        lv_anim_set_var(&a_out, settings_cont);
        lv_anim_set_values(&a_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&a_out, 300);
        lv_anim_set_exec_cb(&a_out, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
        lv_anim_set_ready_cb(&a_out, fade_out_settings_cb);
        lv_anim_start(&a_out);
    }
}

static void fade_out_home_cb(lv_anim_t * a)
{
    lv_obj_t * cont = (lv_obj_t *)a->var;
    lv_obj_del(cont);
    cr_settings_scr();
    lv_obj_set_style_opa(settings_cont, LV_OPA_TRANSP, 0);
    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, settings_cont);
    lv_anim_set_values(&a_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a_in, 300);
    lv_anim_set_exec_cb(&a_in, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&a_in);
}

static void fade_out_settings_cb(lv_anim_t * a)
{
    lv_obj_t * cont = (lv_obj_t *)a->var;
    lv_obj_del(cont);
    cr_home_scr();
    lv_obj_set_style_opa(home_cont, LV_OPA_TRANSP, 0);
    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, home_cont);
    lv_anim_set_values(&a_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a_in, 300);
    lv_anim_set_exec_cb(&a_in, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&a_in);
}

void setup_home_scr(){
    cr_status_bar();
    cr_home_scr();
}

void cr_status_bar() {
    status_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(status_bar, LV_PCT(100), LV_PCT(15));
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 2, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * status_label = lv_label_create(status_bar);
    lv_label_set_text(status_label, "Status: Ready");
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 0, 0);
}

void cr_home_scr(){
    home_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(home_cont, LV_PCT(100), LV_PCT(85));
    lv_obj_clear_flag(home_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(home_cont, 0, 0);
    lv_obj_align_to(home_cont, status_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    home_list = lv_list_create(home_cont);
    lv_obj_set_size(home_list, LV_PCT(100), LV_PCT(100));
    lv_obj_align(home_list, LV_ALIGN_TOP_MID, 0, 0);

    // Add buttons to the list
    lv_obj_t * btn;
    btn = lv_list_add_button(home_list, LV_SYMBOL_SETTINGS, "Settings");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_button(home_list, LV_SYMBOL_CHARGE, "Gemini AI");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_button(home_list, LV_SYMBOL_GPS, "Weather");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
    btn = lv_list_add_button(home_list, LV_SYMBOL_TINT, "About");
    lv_obj_add_event_cb(btn, event_handler, LV_EVENT_CLICKED, NULL);
}

void cr_settings_scr() {
    settings_cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(settings_cont, LV_PCT(100), LV_PCT(85));
    lv_obj_clear_flag(settings_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(settings_cont, 0, 0);
    lv_obj_align_to(settings_cont, status_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    // Example settings content: a label and a back button
    // You can expand this with a list, sliders, toggles, etc., as needed

    lv_obj_t * settings_label = lv_label_create(settings_cont);
    lv_label_set_text(settings_label, "Settings Screen");
    lv_obj_align(settings_label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t * back_btn = lv_btn_create(settings_cont);
    lv_obj_set_size(back_btn, LV_PCT(20), LV_PCT(8));
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 10, -10);

    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "Back");
    lv_obj_center(back_label);

    lv_obj_add_event_cb(back_btn, back_handler, LV_EVENT_CLICKED, NULL);

    // Optional: Add more settings buttons here, e.g.,
    // lv_obj_t * wifi_btn = lv_btn_create(settings_cont);
    // ... and add event handlers if needed
}

void lvgl_task(void *pvParameters) {
    // Create small canvas as touch indicator for custom cursor using points/lines
    touch_ind = lv_canvas_create(lv_scr_act());
    lv_obj_set_size(touch_ind, 20, 20);  // Precise 20x20 size

    // Define buffer for ARGB8888 (true color alpha)
    LV_DRAW_BUF_DEFINE_STATIC(draw_buf, 20, 20, LV_COLOR_FORMAT_ARGB8888);
    LV_DRAW_BUF_INIT_STATIC(draw_buf);
    lv_canvas_set_draw_buf(touch_ind, &draw_buf);

    // Set transparent background
    lv_obj_set_style_bg_opa(touch_ind, LV_OPA_TRANSP, 0);

    // Initialize layer for drawing
    lv_layer_t layer;
    lv_canvas_init_layer(touch_ind, &layer);

    // Draw horizontal line for crosshair cursor
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0xFF0000);
    line_dsc.width = 1;
    line_dsc.round_start = 0;
    line_dsc.round_end = 0;
    line_dsc.p1.x = 0;
    line_dsc.p1.y = 10;
    line_dsc.p2.x = 19;
    line_dsc.p2.y = 10;
    lv_draw_line(&layer, &line_dsc);

    // Draw vertical line for crosshair cursor
    line_dsc.p1.x = 10;
    line_dsc.p1.y = 0;
    line_dsc.p2.x = 10;
    line_dsc.p2.y = 19;
    lv_draw_line(&layer, &line_dsc);

    // Finish layer to apply drawings
    lv_canvas_finish_layer(touch_ind, &layer);

    lv_indev_set_cursor(lv_indev_get_next(NULL), touch_ind);

    setup_home_scr();

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t mount_fatfs(void) {
    ESP_LOGI(TAG, "--- Mounting FAT Filesystem ---");
    const char *base_path = "/storage";
    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = true;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(base_path, "fatfs", &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS partition: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "FATFS mounted successfully.");
    
    ESP_LOGI(TAG, "Listing all files and directories in /storage:");
    DIR *dir = opendir("/storage");
    if (dir) {
        struct dirent *entry;
        int item_count = 0;
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "- %s (type: %d)", entry->d_name, entry->d_type);
            item_count++;
        }
        closedir(dir);
        ESP_LOGI(TAG, "Total items in /storage: %d", item_count);
    } else {
        ESP_LOGE(TAG, "Failed to open directory /storage for listing");
    }
    return ESP_OK;
}

static void init_display(void) {
    lcd.init();
    lcd.setRotation(1);
    lcd.clear(lcd.color565(0, 0, 0));
    lcd.setBrightness(255);

    if (mount_fatfs() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS");
    }
    
    uint16_t caldata[8];
    bool cal_loaded = load_calibration_data(caldata);
    if (!cal_loaded) {
        perform_calibration(caldata);
        save_calibration_data(caldata);
    }
    lcd.setTouchCalibrate(caldata);

    lv_init();
    lv_tick_set_cb(get_tick_ms);

    uint32_t width = lcd.width();
    uint32_t height = lcd.height();
    uint32_t buf_size_bytes = width * height;
    uint32_t *disp_buf1 = (uint32_t *)malloc(buf_size_bytes);
    uint32_t *disp_buf2 = (uint32_t *)malloc(buf_size_bytes);
    if (!disp_buf1 || !disp_buf2) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        vTaskDelete(NULL);
        return;
    }

    lv_display_t *disp = lv_display_create(width, height);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, disp_buf1, disp_buf2, buf_size_bytes * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
    lv_indev_set_disp(indev, disp);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi connected, got IP");
        if (wifi_connected != NULL) {
            xSemaphoreGive(wifi_connected);
        }
    }
}

static void init_console(void) {
    // Configure UART for console I/O
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

    // Initialize console
    esp_console_config_t console_config = {};
    console_config.max_cmdline_length = 256;
    console_config.max_cmdline_args = 8;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    console_config.hint_color = atoi("36"); // ANSI color code for cyan
#endif

    ESP_ERROR_CHECK(esp_console_init(&console_config));

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(10);
    linenoiseAllowEmpty(false);
}

void print_citations(cJSON* metadata) {
    if (!cJSON_HasObjectItem(metadata, "groundingSupports") || !cJSON_HasObjectItem(metadata, "groundingChunks")) {
        return;
    }

    cJSON *supports = cJSON_GetObjectItem(metadata, "groundingSupports");
    cJSON *chunks = cJSON_GetObjectItem(metadata, "groundingChunks");

    if (!cJSON_IsArray(supports) || !cJSON_IsArray(chunks)) {
        return;
    }

    std::set<int> used_indices;
    int num_sup = cJSON_GetArraySize(supports);
    for (int i = 0; i < num_sup; i++) {
        cJSON *sup = cJSON_GetArrayItem(supports, i);
        cJSON *indices = cJSON_GetObjectItem(sup, "groundingChunkIndices");
        if (!indices || !cJSON_IsArray(indices)) continue;
        int num_idx = cJSON_GetArraySize(indices);
        for (int k = 0; k < num_idx; k++) {
            cJSON *i_json = cJSON_GetArrayItem(indices, k);
            if (!i_json || !cJSON_IsNumber(i_json)) continue;
            int idx = i_json->valueint;
            if (idx >= 0 && idx < cJSON_GetArraySize(chunks)) {
                used_indices.insert(idx);
            }
        }
    }

    if (used_indices.empty()) return;

    printf("\nCitations:\n");
    for (int idx : used_indices) {
        cJSON *chunk = cJSON_GetArrayItem(chunks, idx);
        if (!chunk || !cJSON_HasObjectItem(chunk, "web")) continue;
        cJSON *web = cJSON_GetObjectItem(chunk, "web");
        if (!web || !cJSON_HasObjectItem(web, "uri")) continue;
        cJSON *uri_json = cJSON_GetObjectItem(web, "uri");
        if (!uri_json || !cJSON_IsString(uri_json)) continue;
        printf("[%d] %s\n", idx + 1, uri_json->valuestring);
    }
}

void process_data_line(const std::string& line, HttpData* data) {
    if (line.rfind("data: ", 0) != 0) {
        return;  // Skip non-data lines
    }
    std::string json_str = line.substr(6);  // Skip "data: "
    // Trim leading whitespace if any
    size_t start = json_str.find_first_not_of(" \t");
    if (start != std::string::npos) {
        json_str = json_str.substr(start);
    }
    // Trim trailing whitespace if any
    size_t end = json_str.find_last_not_of(" \t");
    if (end != std::string::npos) {
        json_str = json_str.substr(0, end + 1);
    }
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
                            if (thought_flag && cJSON_IsBool(thought_flag) && cJSON_IsTrue(thought_flag)) {
                                if (data->thoughts.empty()) {
                                    printf("Thoughts :\n");
                                }
                                printf("%s", text->valuestring);
                                data->thoughts += text->valuestring;
                            } else {
                                if (data->answer.empty()) {
                                    printf("Answer:\n");
                                }
                                printf("%s", text->valuestring);
                                data->answer += text->valuestring;
                            }
                        }
                    }
                }
            }
            // Handle grounding metadata
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
    // Process any complete lines left in the buffer
    while ((pos = data->response_buffer.find('\n')) != std::string::npos) {
        std::string line = data->response_buffer.substr(0, pos);
        data->response_buffer.erase(0, pos + 1);
        process_data_line(line, data);
    }
    // Handle any trailing incomplete line (e.g., last chunk without \n)
    if (!data->response_buffer.empty()) {
        std::string line = data->response_buffer;
        data->response_buffer.clear();
        process_data_line(line, data);
    }
    ESP_LOGI(TAG, "Stream processing complete. Final thoughts: %zu chars, answer: %zu chars", data->thoughts.length(), data->answer.length());
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
        if (!line) continue;
        if (strlen(line) == 0) {
            free(line);
            continue;
        }

        HttpData data;
        data.response_buffer.reserve(1024);
        data.grounding_metadata = nullptr;

        // Build request JSON with user prompt
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
        snprintf(url, sizeof(url),
                 "https://generativelanguage.googleapis.com/v1beta/models/"
                 "gemini-flash-latest:streamGenerateContent?alt=sse&key=%s",
                 API_KEY);

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
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP POST Status = %d",
                     esp_http_client_get_status_code(client));
            printf("\n");
            if (data.grounding_metadata) {
                print_citations(data.grounding_metadata);
                cJSON_Delete(data.grounding_metadata);
            } else {
                printf("No grounding metadata available.\n");
            }
        } else {
            ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
        free(post_data);
        linenoiseHistoryAdd(line);
        free(line);
    }
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

    init_display();
    
    xTaskCreate(lvgl_task, "lvgl", 20 * 1024, NULL, 5, NULL);
    xTaskCreate(http_task, "http_task", 20 * 1024, NULL, 5, NULL);

    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

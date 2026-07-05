#include "weather_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "zlib.h"
#include <string.h>
#include <algorithm>
#include <cctype>

static const char *TAG = "WeatherManager";

// HTTP 响应缓冲区（分配在 SPIRAM 上，避免占用宝贵的内部 RAM）
static char* response_buffer = NULL;
static int response_len = 0;
static const int RESPONSE_BUFFER_SIZE = 8192;

// GZIP 解压缓冲区（和风天气 API 默认返回 gzip 压缩数据）
static char* decompressed_buffer = NULL;
static const int DECOMPRESSED_BUFFER_SIZE = 8192;

esp_err_t WeatherManager::http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (response_buffer && response_len + evt->data_len < RESPONSE_BUFFER_SIZE - 1) {
                memcpy(response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

WeatherManager::WeatherManager() {
    response_buffer = (char*)heap_caps_malloc(RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    decompressed_buffer = (char*)heap_caps_malloc(DECOMPRESSED_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
}

WeatherManager& WeatherManager::getInstance() {
    static WeatherManager instance;
    return instance;
}

void WeatherManager::setApiConfig(const char* key, const char* host) {
    api_key_ = key;
    api_host_ = host;
}

bool WeatherManager::updateFromExternal(const std::string& city,
                                        const std::string& weather_text,
                                        const std::string& temperature,
                                        const std::string& update_time) {
    if (city.empty() || weather_text.empty() || temperature.empty()) {
        ESP_LOGW(TAG, "外部天气数据无效：city/text/temp 不能为空");
        return false;
    }

    latest_data_.city = city;
    latest_data_.text = weather_text;
    latest_data_.temp = temperature;
    latest_data_.update_time = update_time.empty() ? "mcp" : update_time;
    latest_data_.valid = true;

    ESP_LOGI(TAG, "天气已由外部写入: %s %s %s°C",
             latest_data_.city.c_str(),
             latest_data_.text.c_str(),
             latest_data_.temp.c_str());
    return true;
}

// GZIP 安全解压（和风天气 API 返回 gzip 格式）
static bool decompress_gzip_safe(const uint8_t* src, int src_len, char* dst, int dst_max_len, int* out_len) {
    if (src_len < 18 || src[0] != 0x1f || src[1] != 0x8b) return false;
    z_stream strm = {};
    strm.next_in = (Bytef*)src;
    strm.avail_in = src_len;
    strm.next_out = (Bytef*)dst;
    strm.avail_out = dst_max_len - 1;
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return false;
    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END && ret != Z_OK) return false;
    *out_len = dst_max_len - 1 - strm.avail_out;
    dst[*out_len] = '\0';
    return true;
}

// 判断城市名是否可用于 UI 展示（排除 "Ip" 这类占位值）
static bool is_valid_display_city(const char* city) {
    if (city == nullptr || city[0] == '\0') {
        return false;
    }
    std::string normalized(city);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), ::isspace), normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return normalized != "ip" && normalized != "auto_ip" && normalized != "unknown";
}

// 优先使用更像“真实地名”的字段，避免把 "Ip" 显示到页面上
static std::string pick_city_name_for_display(cJSON* first_city, const std::string& fallback_city) {
    const char* keys[] = {"adm2", "adm1", "name"};
    for (const char* key : keys) {
        cJSON* item = cJSON_GetObjectItem(first_city, key);
        if (item && cJSON_IsString(item) && is_valid_display_city(item->valuestring)) {
            return item->valuestring;
        }
    }
    return fallback_city;
}

bool WeatherManager::update() {
    if (!response_buffer) {
        ESP_LOGW(TAG, "天气缓冲区未分配");
        return false;
    }

    // 使用 wttr.in 获取天气数据
    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);
    
    ESP_LOGI(TAG, "正在通过 wttr.in 获取天气数据...");
    esp_http_client_config_t weather_config = {};
    weather_config.url = "http://wttr.in/?format=j1";
    weather_config.event_handler = http_event_handler;
    weather_config.timeout_ms = 15000;
    esp_http_client_handle_t client = esp_http_client_init(&weather_config);
    
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    bool success = false;

    if (err == ESP_OK && status_code == 200 && response_len > 0) {
        if (response_len < RESPONSE_BUFFER_SIZE) {
            response_buffer[response_len] = '\0';
        } else {
            response_buffer[RESPONSE_BUFFER_SIZE - 1] = '\0';
        }

        cJSON *root = cJSON_Parse(response_buffer);
        if (root) {
            cJSON *current_condition_arr = cJSON_GetObjectItem(root, "current_condition");
            cJSON *nearest_area_arr = cJSON_GetObjectItem(root, "nearest_area");

            if (cJSON_IsArray(current_condition_arr) && cJSON_IsArray(nearest_area_arr)) {
                cJSON *current_condition = cJSON_GetArrayItem(current_condition_arr, 0);
                cJSON *nearest_area = cJSON_GetArrayItem(nearest_area_arr, 0);

                if (current_condition && nearest_area) {
                    cJSON *temp_C = cJSON_GetObjectItem(current_condition, "temp_C");
                    cJSON *weatherDesc_arr = cJSON_GetObjectItem(current_condition, "weatherDesc");
                    cJSON *areaName_arr = cJSON_GetObjectItem(nearest_area, "areaName");

                    if (cJSON_IsString(temp_C) && cJSON_IsArray(weatherDesc_arr) && cJSON_IsArray(areaName_arr)) {
                        cJSON *weatherDesc = cJSON_GetArrayItem(weatherDesc_arr, 0);
                        cJSON *areaName = cJSON_GetArrayItem(areaName_arr, 0);

                        if (weatherDesc && areaName) {
                            cJSON *weatherDesc_val = cJSON_GetObjectItem(weatherDesc, "value");
                            cJSON *areaName_val = cJSON_GetObjectItem(areaName, "value");

                            if (cJSON_IsString(weatherDesc_val) && cJSON_IsString(areaName_val)) {
                                latest_data_.temp = temp_C->valuestring;
                                latest_data_.text = weatherDesc_val->valuestring;
                                latest_data_.city = areaName_val->valuestring;
                                latest_data_.valid = true;
                                success = true;
                                ESP_LOGI(TAG, "wttr.in 天气更新成功: %s, %s°C, %s",
                                         latest_data_.city.c_str(), latest_data_.temp.c_str(), latest_data_.text.c_str());
                            }
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGE(TAG, "天气请求失败 (err=%d, status=%d)", err, status_code);
    }
    esp_http_client_cleanup(client);
    return success;
}

void WeatherManager::parseWeatherJson(const char* json_data) {
    cJSON *root = cJSON_Parse(json_data);
    if (!root) return;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && strcmp(code->valuestring, "200") == 0) {
        cJSON *now = cJSON_GetObjectItem(root, "now");
        if (now) {
            latest_data_.temp = cJSON_GetObjectItem(now, "temp")->valuestring;
            latest_data_.text = cJSON_GetObjectItem(now, "text")->valuestring;
            latest_data_.valid = true;
            ESP_LOGI(TAG, "天气更新成功: %s°C, %s", latest_data_.temp.c_str(), latest_data_.text.c_str());
        }
    }
    cJSON_Delete(root);
}

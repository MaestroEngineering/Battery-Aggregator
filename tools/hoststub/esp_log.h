#pragma once
#include <stdio.h>
extern int g_hoststub_log;  // 0 = quiet
#define ESP_LOG_X(l, tag, fmt, ...) do { if (g_hoststub_log) printf("%s %s: " fmt "\n", l, tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) ESP_LOG_X("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_X("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_X("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do {} while (0)

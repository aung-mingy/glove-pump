#include "stubs.h"

#define ESP_LOGI(tag, fmt, ...) ((void)(tag), printf("[I] " fmt "\n", ##__VA_ARGS__))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag), printf("[W] " fmt "\n", ##__VA_ARGS__))
#define ESP_LOGE(tag, fmt, ...) ((void)(tag), printf("[E] " fmt "\n", ##__VA_ARGS__))

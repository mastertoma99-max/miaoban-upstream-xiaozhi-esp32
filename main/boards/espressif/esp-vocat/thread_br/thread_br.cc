#include "thread_br.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_psram.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"

namespace {
constexpr char kTag[] = "MiaobanThread";
constexpr char kStorage[] = "ot_nvs";
constexpr size_t kExpectedPsram = 8 * 1024 * 1024;
constexpr EventBits_t kStopped = BIT0;
std::mutex s_lifecycle_mutex;
TaskHandle_t s_supervisor = nullptr;
EventGroupHandle_t s_lifecycle_events = nullptr;
std::atomic<bool> s_stop_requested{false};
// Owned by the supervisor task; caller waits for kStopped before deleting it.
bool s_ot_started = false;
bool s_br_initialized = false;
bool s_eventfd_registered = false;
bool s_mdns_owned = false;

void LogMemory(const char* phase) {
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(kTag,
             "MEM phase=%s internal_free=%u internal_min=%u internal_largest=%u "
             "psram_free=%u psram_min=%u psram_largest=%u supervisor_stack_free=%u",
             phase, static_cast<unsigned>(heap_caps_get_free_size(internal)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(internal)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(internal)),
             static_cast<unsigned>(heap_caps_get_free_size(external)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(external)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(external)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

esp_err_t StartThread(esp_netif_t* backbone) {
    // Keep Thread credentials separate from the voice application's NVS partition.
    ESP_RETURN_ON_ERROR(nvs_flash_init_partition(kStorage), kTag, "Thread NVS init failed");
    esp_vfs_eventfd_config_t eventfd_config = {.max_fds = 3};
    ESP_RETURN_ON_ERROR(esp_vfs_eventfd_register(&eventfd_config), kTag, "eventfd init failed");
    s_eventfd_registered = true;

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (err == ESP_OK) {
        s_mdns_owned = true;
        uint8_t mac[6];
        ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), kTag, "MAC read failed");
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "miaoban-%02x%02x%02x", mac[3], mac[4], mac[5]);
        ESP_RETURN_ON_ERROR(mdns_hostname_set(hostname), kTag, "mDNS hostname failed");
    }

    // Set the existing Wi-Fi station as backbone before initializing OpenThread.
    // Do not create another Wi-Fi driver, event loop, or network connection.
    ESP_RETURN_ON_ERROR(esp_netif_create_ip6_linklocal(backbone), kTag, "Wi-Fi IPv6 init failed");
    esp_openthread_set_backbone_netif(backbone);
    esp_openthread_config_t config = {};
    const esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_OPENTHREAD();
    config.netif_config = netif_config;
    auto& platform = config.platform_config;
    platform.radio_config.radio_mode = RADIO_MODE_UART_RCP;
    auto& uart = platform.radio_config.radio_uart_config;
    uart.port = UART_NUM_2;
    uart.uart_config.baud_rate = 460800;
    uart.uart_config.data_bits = UART_DATA_8_BITS;
    uart.uart_config.parity = UART_PARITY_DISABLE;
    uart.uart_config.stop_bits = UART_STOP_BITS_1;
    uart.uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart.uart_config.source_clk = UART_SCLK_DEFAULT;
    uart.rx_pin = static_cast<gpio_num_t>(CONFIG_MIAOBAN_THREAD_RX_GPIO);
    uart.tx_pin = static_cast<gpio_num_t>(CONFIG_MIAOBAN_THREAD_TX_GPIO);
    platform.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
    platform.port_config.storage_partition_name = kStorage;
    platform.port_config.netif_queue_size = 10;
    platform.port_config.task_queue_size = 10;

    LogMemory("before_thread");
    ESP_RETURN_ON_ERROR(esp_openthread_start(&config), kTag, "OpenThread start failed");
    s_ot_started = true;
    esp_openthread_lock_acquire(portMAX_DELAY);
    err = esp_openthread_border_router_init();
    if (err == ESP_OK) {
        s_br_initialized = true;
        otInstance* instance = esp_openthread_get_instance();
        otOperationalDatasetTlvs tlvs;
        otError ot_error = otDatasetGetActiveTlvs(instance, &tlvs);
        if (ot_error == OT_ERROR_NOT_FOUND) {
            // Generate unique credentials; never use a shared example network key.
            otOperationalDataset dataset = {};
            ot_error = otDatasetCreateNewNetwork(instance, &dataset);
            if (ot_error == OT_ERROR_NONE) {
                strlcpy(dataset.mNetworkName.m8, "Miaoban", sizeof(dataset.mNetworkName.m8));
                ot_error = otDatasetSetActive(instance, &dataset);
            }
            if (ot_error == OT_ERROR_NONE) {
                ot_error = otDatasetGetActiveTlvs(instance, &tlvs);
            }
        }
        err = ot_error == OT_ERROR_NONE ? esp_openthread_auto_start(&tlvs) : ESP_FAIL;
    }
    esp_openthread_lock_release();
    ESP_RETURN_ON_ERROR(err, kTag, "BR/dataset initialization failed");
    LogMemory("after_thread");
    return ESP_OK;
}

void ReleaseThread() {
    if (s_ot_started) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        if (s_br_initialized) {
            ESP_ERROR_CHECK(esp_openthread_border_router_deinit());
            s_br_initialized = false;
        }
        otInstance* instance = esp_openthread_get_instance();
        otThreadSetEnabled(instance, false);
        otIp6SetEnabled(instance, false);
        esp_openthread_lock_release();
        ESP_ERROR_CHECK(esp_openthread_stop());
        s_ot_started = false;
        esp_openthread_set_backbone_netif(nullptr);
    }
    if (s_mdns_owned) {
        mdns_free();
        s_mdns_owned = false;
    }
    if (s_eventfd_registered) {
        ESP_ERROR_CHECK(esp_vfs_eventfd_unregister());
        s_eventfd_registered = false;
    }
}

void Supervisor(void*) {
    ESP_LOGI(kTag, "S3 N16R8 + H2 UART RCP: TX=%d RX=%d, 460800 8N1", CONFIG_MIAOBAN_THREAD_TX_GPIO,
             CONFIG_MIAOBAN_THREAD_RX_GPIO);
    if (esp_psram_get_size() != kExpectedPsram) {
        ESP_LOGW(kTag, "Capacity profile expects 8 MiB PSRAM; detected %u bytes",
                 static_cast<unsigned>(esp_psram_get_size()));
    }
    LogMemory("waiting_wifi");
    esp_netif_t* backbone = nullptr;
    esp_netif_ip_info_t ip = {};
    while (!s_stop_requested.load()) {
        backbone = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (backbone && esp_netif_is_netif_up(backbone) &&
            esp_netif_get_ip_info(backbone, &ip) == ESP_OK && ip.ip.addr != 0) {
            break;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
    if (!s_stop_requested.load()) {
        // Ordinary Wi-Fi reconnects retain STA. Explicit StopStation destroys it,
        // so WifiBoard first waits for this service to release the interface.
        esp_err_t err = StartThread(backbone);
        if (err == ESP_OK) {
            while (!s_stop_requested.load()) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
                esp_openthread_lock_acquire(portMAX_DELAY);
                const otDeviceRole role = otThreadGetDeviceRole(esp_openthread_get_instance());
                esp_openthread_lock_release();
                ESP_LOGI(kTag, "Thread role=%s", otThreadDeviceRoleToString(role));
                LogMemory("running");
            }
        } else {
            ESP_LOGE(kTag, "BR setup failed: %s", esp_err_to_name(err));
        }
    }
    ReleaseThread();
    xEventGroupSetBits(s_lifecycle_events, kStopped);
    // Keep the handle valid until the owner acknowledges completion and deletes it.
    vTaskSuspend(nullptr);
    vTaskDelete(nullptr);
}
}  // namespace

void StartMiaobanThreadBorderRouter() {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    if (!s_lifecycle_events) {
        s_lifecycle_events = xEventGroupCreate();
    }
    if (!s_lifecycle_events) {
        ESP_LOGE(kTag, "Could not allocate BR lifecycle events");
        return;
    }
    if (s_supervisor) {
        if (!(xEventGroupGetBits(s_lifecycle_events) & kStopped)) {
            return;
        }
        vTaskDelete(s_supervisor);
        s_supervisor = nullptr;
    }
    xEventGroupClearBits(s_lifecycle_events, kStopped);
    s_stop_requested.store(false);
    if (xTaskCreate(Supervisor, "miaoban_br", 6144, nullptr, 3, &s_supervisor) != pdPASS) {
        s_supervisor = nullptr;
        ESP_LOGE(kTag, "Could not allocate BR supervisor task");
    }
}

bool StopMiaobanThreadBorderRouter() {
    std::lock_guard<std::mutex> lock(s_lifecycle_mutex);
    if (!s_supervisor) {
        return true;
    }
    if (!(xEventGroupGetBits(s_lifecycle_events) & kStopped)) {
        s_stop_requested.store(true);
        xTaskNotifyGive(s_supervisor);
        if (!(xEventGroupWaitBits(s_lifecycle_events, kStopped, pdFALSE, pdTRUE,
                                  pdMS_TO_TICKS(2000)) &
              kStopped)) {
            return false;
        }
    }
    vTaskDelete(s_supervisor);
    s_supervisor = nullptr;
    return true;
}

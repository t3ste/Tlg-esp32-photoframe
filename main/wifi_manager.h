#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_update_hostname(void);
// Toggle between full-RX performance (WIFI_PS_NONE, low latency / fast web UI)
// and modem power save (WIFI_PS_MIN_MODEM). Idempotent; safe to call every
// second. The policy for when to use which lives in power_manager.
esp_err_t wifi_manager_set_performance_mode(bool enable);
// Apply the configured IP mode to the STA netif (static address or DHCP).
// Called automatically by wifi_manager_connect; exposed for the provisioning
// connection test, which drives esp_wifi directly (#43).
esp_err_t wifi_manager_apply_ip_config(void);
// Blocks until connected, definitively failed, or timeout_ms elapses - never
// longer, unlike the old unbounded wait (see wifi_manager.c for why that
// could hang forever on a DHCP stall). Returns ESP_ERR_TIMEOUT if neither
// happens in time.
esp_err_t wifi_manager_connect(const char *ssid, const char *password, int timeout_ms);
esp_err_t wifi_manager_disconnect(void);
// True if the most recent wifi_manager_connect() failure's disconnect reason
// (WIFI_EVENT_STA_DISCONNECTED) is one the AP itself uses specifically to
// reject a wrong password/security mismatch (a failed 4-way handshake, MIC
// failure, or an explicit auth-fail code) - as opposed to a merely transient
// failure (AP not currently found/visible, beacon timeout, general
// connection failure) that a retry might well recover from on its own.
// Meaningless if the last connect attempt actually succeeded.
bool wifi_manager_last_failure_is_credential_reject(void);
// Overrides the default reconnect-attempt budget (5) used by the
// WIFI_EVENT_STA_DISCONNECTED handler before it gives up and reports
// WIFI_FAIL_BIT. Takes effect on the next wifi_manager_connect() call.
void wifi_manager_set_max_retries(int max_retries);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip(char *ip_str, size_t len);
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_manager_load_credentials(char *ssid, char *password);
esp_err_t wifi_manager_load_credentials_from_sdcard(char *ssid, char *password);
EventGroupHandle_t wifi_manager_get_event_group(void);
int wifi_manager_scan(wifi_ap_record_t *results, int max_results);

#endif

#ifndef ZECTRIX_WIFI_H_
#define ZECTRIX_WIFI_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

class ZectrixWifi {
public:
    static constexpr size_t kMaxProfiles = 8;
    static constexpr size_t kSsidMaxLen = 32;
    static constexpr size_t kPasswordMaxLen = 64;

    struct Profile {
        char ssid[kSsidMaxLen + 1] = {};
        char password[kPasswordMaxLen + 1] = {};
        bool last_ok = false;
    };

    struct SoftApInfo {
        char ssid[kSsidMaxLen + 1] = {};
        char password[16] = {};
        char url[32] = {};
    };

    enum class ProvisionOutcome {
        kIdle,
        kWaiting,
        kSuccess,
        kCancelled,
        kTimeout,
        kFail,
    };

    ZectrixWifi();
    ~ZectrixWifi();

    esp_err_t Init();

    size_t ProfileCount() const { return profile_count_; }
    int CurrentIndex() const { return current_index_; }
    bool GetProfile(size_t index, Profile* out) const;
    esp_err_t DeleteProfile(size_t index);
    esp_err_t UpsertProfile(const char* ssid, const char* password,
                            size_t* out_index = nullptr);

    esp_err_t ConnectProfile(size_t index, uint32_t timeout_ms);
    esp_err_t Disconnect();
    bool IsConnected() const { return connected_; }
    esp_err_t GetConnectionInfo(char* ssid, size_t ssid_len, char* ip,
                                size_t ip_len) const;

    esp_err_t StartProvisioning(SoftApInfo* info);
    /** Non-blocking status for the SoftAP portal session. */
    ProvisionOutcome PollProvisioning();
    esp_err_t ApplyProvisionedProfile(size_t* out_index = nullptr);
    esp_err_t StopProvisioning();
    ProvisionOutcome provision_outcome() const {
        return provision_outcome_.load();
    }
    const char* provision_ssid() const { return provision_ssid_; }
    const SoftApInfo& softap_info() const { return softap_; }

private:
    static constexpr EventBits_t kConnectedBit = BIT0;
    static constexpr EventBits_t kFailBit = BIT1;
    static constexpr EventBits_t kProvisionedBit = BIT2;

    static void EventHandler(void* arg, esp_event_base_t base, int32_t id,
                             void* data);
    static esp_err_t HttpGetRoot(httpd_req_t* req);
    static esp_err_t HttpPostSave(httpd_req_t* req);
    static esp_err_t HttpCaptive(httpd_req_t* req);

    esp_err_t EnsureStack();
    esp_err_t LoadProfiles();
    esp_err_t SaveProfiles();
    esp_err_t ConnectSta(const char* ssid, const char* password,
                         uint32_t timeout_ms);
    esp_err_t BuildPortalPage(char* html, size_t html_size);
    void HandleWifiEvent(int32_t id, void* data);
    void HandleIpEvent(int32_t id);
    static ZectrixWifi* FromRequest(httpd_req_t* req);
    static void UrlDecode(char* text);

    Profile profiles_[kMaxProfiles] = {};
    size_t profile_count_ = 0;
    int current_index_ = -1;
    bool stack_ready_ = false;
    bool connected_ = false;
    bool provisioning_ = false;
    SoftApInfo softap_ = {};
    char provision_ssid_[kSsidMaxLen + 1] = {};
    char provision_password_[kPasswordMaxLen + 1] = {};
    std::atomic<ProvisionOutcome> provision_outcome_{ProvisionOutcome::kIdle};
    EventGroupHandle_t events_ = nullptr;
    esp_netif_t* sta_netif_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    httpd_handle_t httpd_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;
};

#endif  // ZECTRIX_WIFI_H_

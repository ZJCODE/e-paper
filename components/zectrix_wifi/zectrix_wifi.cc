#include "zectrix_wifi.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "zectrix_wifi";
constexpr char kNvsNamespace[] = "zwifi";
constexpr char kKeyCount[] = "count";
constexpr char kKeyCurrent[] = "cur";

void CopyString(char* dest, size_t dest_size, const char* src) {
    if (dest == nullptr || dest_size == 0) {
        return;
    }
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    std::snprintf(dest, dest_size, "%s", src);
}

bool IsPrintableSsid(const char* ssid) {
    if (ssid == nullptr || ssid[0] == '\0') {
        return false;
    }
    for (const char* p = ssid; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return std::strlen(ssid) <= ZectrixWifi::kSsidMaxLen;
}

}  // namespace

ZectrixWifi::ZectrixWifi() = default;

ZectrixWifi::~ZectrixWifi() {
    StopProvisioning();
    if (wifi_handler_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_handler_);
        wifi_handler_ = nullptr;
    }
    if (ip_handler_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              ip_handler_);
        ip_handler_ = nullptr;
    }
    if (events_ != nullptr) {
        vEventGroupDelete(events_);
        events_ = nullptr;
    }
}

esp_err_t ZectrixWifi::Init() {
    esp_err_t err = EnsureStack();
    if (err != ESP_OK) {
        return err;
    }
    return LoadProfiles();
}

esp_err_t ZectrixWifi::EnsureStack() {
    if (stack_ready_) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (sta_netif_ == nullptr) {
        sta_netif_ = esp_netif_create_default_wifi_sta();
    }
    if (ap_netif_ == nullptr) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (events_ == nullptr) {
        events_ = xEventGroupCreate();
        if (events_ == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (wifi_handler_ == nullptr) {
        err = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &EventHandler, this, &wifi_handler_);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (ip_handler_ == nullptr) {
        err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler, this, &ip_handler_);
        if (err != ESP_OK) {
            return err;
        }
    }

    stack_ready_ = true;
    return ESP_OK;
}

void ZectrixWifi::EventHandler(void* arg, esp_event_base_t base, int32_t id,
                               void* data) {
    auto* self = static_cast<ZectrixWifi*>(arg);
    if (base == WIFI_EVENT) {
        self->HandleWifiEvent(id, data);
    } else if (base == IP_EVENT) {
        self->HandleIpEvent(id);
    }
}

void ZectrixWifi::HandleWifiEvent(int32_t id, void* data) {
    if (id == WIFI_EVENT_STA_START) {
        // Connect is requested explicitly by ConnectSta / ConnectProfile.
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        connected_ = false;
        if (events_ != nullptr) {
            xEventGroupSetBits(events_, kFailBit);
        }
        const auto* event =
            static_cast<wifi_event_sta_disconnected_t*>(data);
        ESP_LOGW(kTag, "STA disconnected, reason=%d",
                 event != nullptr ? event->reason : -1);
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(kTag, "phone joined SoftAP");
    }
}

void ZectrixWifi::HandleIpEvent(int32_t id) {
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    connected_ = true;
    if (events_ != nullptr) {
        xEventGroupSetBits(events_, kConnectedBit);
    }
}

esp_err_t ZectrixWifi::LoadProfiles() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        profile_count_ = 0;
        current_index_ = -1;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t count = 0;
    uint8_t current = 0xff;
    nvs_get_u8(handle, kKeyCount, &count);
    nvs_get_u8(handle, kKeyCurrent, &current);
    if (count > kMaxProfiles) {
        count = kMaxProfiles;
    }

    profile_count_ = 0;
    for (uint8_t i = 0; i < count; ++i) {
        char key[16];
        size_t len = sizeof(profiles_[profile_count_].ssid);
        std::snprintf(key, sizeof(key), "s%u", i);
        err = nvs_get_str(handle, key, profiles_[profile_count_].ssid, &len);
        if (err != ESP_OK) {
            continue;
        }
        len = sizeof(profiles_[profile_count_].password);
        std::snprintf(key, sizeof(key), "p%u", i);
        err = nvs_get_str(handle, key, profiles_[profile_count_].password, &len);
        if (err != ESP_OK) {
            profiles_[profile_count_].password[0] = '\0';
        }
        uint8_t ok = 0;
        std::snprintf(key, sizeof(key), "o%u", i);
        nvs_get_u8(handle, key, &ok);
        profiles_[profile_count_].last_ok = ok != 0;
        ++profile_count_;
    }
    current_index_ = (current < profile_count_) ? static_cast<int>(current) : -1;
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t ZectrixWifi::SaveProfiles() {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, kKeyCount, static_cast<uint8_t>(profile_count_));
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, kKeyCurrent,
                         current_index_ < 0
                             ? 0xff
                             : static_cast<uint8_t>(current_index_));
    }
    for (size_t i = 0; err == ESP_OK && i < profile_count_; ++i) {
        char key[16];
        std::snprintf(key, sizeof(key), "s%u", static_cast<unsigned>(i));
        err = nvs_set_str(handle, key, profiles_[i].ssid);
        if (err != ESP_OK) {
            break;
        }
        std::snprintf(key, sizeof(key), "p%u", static_cast<unsigned>(i));
        err = nvs_set_str(handle, key, profiles_[i].password);
        if (err != ESP_OK) {
            break;
        }
        std::snprintf(key, sizeof(key), "o%u", static_cast<unsigned>(i));
        err = nvs_set_u8(handle, key, profiles_[i].last_ok ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool ZectrixWifi::GetProfile(size_t index, Profile* out) const {
    if (out == nullptr || index >= profile_count_) {
        return false;
    }
    *out = profiles_[index];
    return true;
}

esp_err_t ZectrixWifi::DeleteProfile(size_t index) {
    if (index >= profile_count_) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = index; i + 1 < profile_count_; ++i) {
        profiles_[i] = profiles_[i + 1];
    }
    --profile_count_;
    if (current_index_ == static_cast<int>(index)) {
        current_index_ = -1;
    } else if (current_index_ > static_cast<int>(index)) {
        --current_index_;
    }
    return SaveProfiles();
}

esp_err_t ZectrixWifi::UpsertProfile(const char* ssid, const char* password,
                                     size_t* out_index) {
    if (!IsPrintableSsid(ssid) || password == nullptr ||
        std::strlen(password) > kPasswordMaxLen) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < profile_count_; ++i) {
        if (std::strcmp(profiles_[i].ssid, ssid) == 0) {
            CopyString(profiles_[i].password, sizeof(profiles_[i].password),
                       password);
            profiles_[i].last_ok = false;
            current_index_ = static_cast<int>(i);
            if (out_index != nullptr) {
                *out_index = i;
            }
            return SaveProfiles();
        }
    }
    if (profile_count_ >= kMaxProfiles) {
        return ESP_ERR_NO_MEM;
    }
    const size_t index = profile_count_;
    CopyString(profiles_[index].ssid, sizeof(profiles_[index].ssid), ssid);
    CopyString(profiles_[index].password, sizeof(profiles_[index].password),
               password);
    profiles_[index].last_ok = false;
    ++profile_count_;
    current_index_ = static_cast<int>(index);
    if (out_index != nullptr) {
        *out_index = index;
    }
    return SaveProfiles();
}

esp_err_t ZectrixWifi::ConnectSta(const char* ssid, const char* password,
                                  uint32_t timeout_ms) {
    esp_err_t err = EnsureStack();
    if (err != ESP_OK) {
        return err;
    }
    if (events_ != nullptr) {
        xEventGroupClearBits(events_, kConnectedBit | kFailBit);
    }
    connected_ = false;

    wifi_config_t config = {};
    CopyString(reinterpret_cast<char*>(config.sta.ssid), sizeof(config.sta.ssid),
               ssid);
    CopyString(reinterpret_cast<char*>(config.sta.password),
               sizeof(config.sta.password), password);
    config.sta.threshold.authmode =
        password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(provisioning_ ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        return err;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        return err;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        events_, kConnectedBit | kFailBit, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    if ((bits & kConnectedBit) != 0) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t ZectrixWifi::ConnectProfile(size_t index, uint32_t timeout_ms) {
    if (index >= profile_count_) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t err =
        ConnectSta(profiles_[index].ssid, profiles_[index].password, timeout_ms);
    profiles_[index].last_ok = err == ESP_OK;
    current_index_ = static_cast<int>(index);
    SaveProfiles();
    return err;
}

esp_err_t ZectrixWifi::Disconnect() {
    connected_ = false;
    const esp_err_t err = esp_wifi_disconnect();
    return err == ESP_ERR_WIFI_NOT_STARTED ? ESP_OK : err;
}

esp_err_t ZectrixWifi::GetConnectionInfo(char* ssid, size_t ssid_len, char* ip,
                                         size_t ip_len) const {
    if (ssid != nullptr && ssid_len > 0) {
        ssid[0] = '\0';
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            CopyString(ssid, ssid_len, reinterpret_cast<const char*>(ap.ssid));
        } else if (current_index_ >= 0) {
            CopyString(ssid, ssid_len, profiles_[current_index_].ssid);
        }
    }
    if (ip != nullptr && ip_len > 0) {
        ip[0] = '\0';
        if (sta_netif_ != nullptr) {
            esp_netif_ip_info_t info = {};
            if (esp_netif_get_ip_info(sta_netif_, &info) == ESP_OK &&
                info.ip.addr != 0) {
                std::snprintf(ip, ip_len, IPSTR, IP2STR(&info.ip));
            }
        }
    }
    return ESP_OK;
}

ZectrixWifi* ZectrixWifi::FromRequest(httpd_req_t* req) {
    return static_cast<ZectrixWifi*>(req->user_ctx);
}

void ZectrixWifi::UrlDecode(char* text) {
    if (text == nullptr) {
        return;
    }
    char* src = text;
    char* dst = text;
    while (*src != '\0') {
        if (src[0] == '+' ) {
            *dst++ = ' ';
            ++src;
        } else if (src[0] == '%' && src[1] != '\0' && src[2] != '\0') {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(src[1]);
            const int lo = hex(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = static_cast<char>((hi << 4) | lo);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

esp_err_t ZectrixWifi::BuildPortalPage(char* html, size_t html_size) {
    if (html == nullptr || html_size < 256) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan = {};
    scan.show_hidden = false;
    scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    uint16_t count = 0;
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&count);
    }
    if (count > 24) {
        count = 24;
    }
    wifi_ap_record_t records[24] = {};
    if (count > 0) {
        esp_wifi_scan_get_ap_records(&count, records);
    }

    size_t used = 0;
    auto append = [&](const char* chunk) -> bool {
        const size_t len = std::strlen(chunk);
        if (used + len + 1 > html_size) {
            return false;
        }
        std::memcpy(html + used, chunk, len + 1);
        used += len;
        return true;
    };

    if (!append(
            "<!DOCTYPE html><html><head><meta charset=utf-8>"
            "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
            "<title>NOTE4 Wi-Fi</title>"
            "<style>body{font-family:sans-serif;margin:16px;max-width:420px}"
            "label,li{display:block;margin:8px 0}input[type=password],"
            "input[type=text]{width:100%;padding:8px;box-sizing:border-box}"
            "button{padding:10px 16px;margin-top:12px}</style></head><body>"
            "<h1>Add Wi-Fi</h1>"
            "<p>Select a 2.4 GHz network, enter the password, then save.</p>"
            "<form method=POST action=/save>")) {
        return ESP_ERR_NO_MEM;
    }

    if (count == 0) {
        if (!append("<p>No networks found. Enter SSID manually.</p>"
                    "<label>SSID <input name=ssid required></label>")) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        if (!append("<fieldset><legend>Nearby networks</legend>")) {
            return ESP_ERR_NO_MEM;
        }
        bool checked = false;
        for (uint16_t i = 0; i < count; ++i) {
            const char* ssid = reinterpret_cast<const char*>(records[i].ssid);
            if (!IsPrintableSsid(ssid) || std::strchr(ssid, '"') != nullptr ||
                std::strchr(ssid, '<') != nullptr ||
                std::strchr(ssid, '&') != nullptr) {
                continue;
            }
            char line[160];
            std::snprintf(line, sizeof(line),
                          "<label><input type=radio name=ssid value=\"%s\" %s> "
                          "%s (%d dBm)</label>",
                          ssid, checked ? "" : "checked", ssid,
                          records[i].rssi);
            checked = true;
            if (!append(line)) {
                return ESP_ERR_NO_MEM;
            }
        }
        if (!append("</fieldset>"
                    "<label>Or type SSID "
                    "<input name=ssid_manual placeholder=\"optional\"></label>")) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!append("<label>Password <input type=password name=password></label>"
                "<button type=submit>Save and Connect</button></form>"
                "<p><a href=/>Refresh scan</a></p></body></html>")) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ZectrixWifi::HttpGetRoot(httpd_req_t* req) {
    auto* self = FromRequest(req);
    if (self == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no wifi");
    }
    // Large enough for ~24 AP rows plus markup.
    static char page[12 * 1024];
    const esp_err_t err = self->BuildPortalPage(page, sizeof(page));
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "scan failed");
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t ZectrixWifi::HttpCaptive(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t ZectrixWifi::HttpPostSave(httpd_req_t* req) {
    auto* self = FromRequest(req);
    if (self == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no wifi");
    }

    char body[256] = {};
    const int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
    }
    body[received] = '\0';

    char ssid[kSsidMaxLen + 1] = {};
    char manual[kSsidMaxLen + 1] = {};
    char password[kPasswordMaxLen + 1] = {};
    char* cursor = body;
    while (cursor != nullptr && *cursor != '\0') {
        char* pair = cursor;
        char* next = std::strchr(cursor, '&');
        if (next != nullptr) {
            *next = '\0';
            cursor = next + 1;
        } else {
            cursor = nullptr;
        }
        char* eq = std::strchr(pair, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        char* value = eq + 1;
        UrlDecode(value);
        if (std::strcmp(pair, "ssid") == 0) {
            CopyString(ssid, sizeof(ssid), value);
        } else if (std::strcmp(pair, "ssid_manual") == 0) {
            CopyString(manual, sizeof(manual), value);
        } else if (std::strcmp(pair, "password") == 0) {
            CopyString(password, sizeof(password), value);
        }
    }
    if (manual[0] != '\0') {
        CopyString(ssid, sizeof(ssid), manual);
    }
    if (!IsPrintableSsid(ssid)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ssid");
    }

    CopyString(self->provision_ssid_, sizeof(self->provision_ssid_), ssid);
    CopyString(self->provision_password_, sizeof(self->provision_password_),
               password);
    self->provision_outcome_.store(ProvisionOutcome::kSuccess);
    if (self->events_ != nullptr) {
        xEventGroupSetBits(self->events_, kProvisionedBit);
    }

    constexpr char kOkPage[] =
        "<!DOCTYPE html><html><body><h1>Saved</h1>"
        "<p>Return to the NOTE4 screen.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kOkPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t ZectrixWifi::StartProvisioning(SoftApInfo* info) {
    esp_err_t err = EnsureStack();
    if (err != ESP_OK) {
        return err;
    }
    if (provisioning_) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    std::snprintf(softap_.ssid, sizeof(softap_.ssid), "NOTE4-%02X%02X", mac[4],
                  mac[5]);
    constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    uint32_t rnd = esp_random();
    for (int i = 0; i < 8; ++i) {
        softap_.password[i] = kAlphabet[(rnd + i * 17) % (sizeof(kAlphabet) - 1)];
        rnd = (rnd >> 3) ^ esp_random();
    }
    softap_.password[8] = '\0';
    CopyString(softap_.url, sizeof(softap_.url), "http://192.168.4.1");

    wifi_config_t ap_config = {};
    CopyString(reinterpret_cast<char*>(ap_config.ap.ssid),
               sizeof(ap_config.ap.ssid), softap_.ssid);
    CopyString(reinterpret_cast<char*>(ap_config.ap.password),
               sizeof(ap_config.ap.password), softap_.password);
    ap_config.ap.ssid_len = std::strlen(softap_.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection = 4;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        return err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    err = httpd_start(&httpd_, &config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_uri_t root = {};
    root.uri = "/";
    root.method = HTTP_GET;
    root.handler = HttpGetRoot;
    root.user_ctx = this;
    httpd_uri_t save = {};
    save.uri = "/save";
    save.method = HTTP_POST;
    save.handler = HttpPostSave;
    save.user_ctx = this;
    httpd_register_uri_handler(httpd_, &root);
    httpd_register_uri_handler(httpd_, &save);
    const char* captive_paths[] = {
        "/generate_204", "/gen_204", "/hotspot-detect.html",
        "/library/test/success.html", "/ncsi.txt", "/connecttest.txt"};
    for (const char* path : captive_paths) {
        httpd_uri_t captive = {};
        captive.uri = path;
        captive.method = HTTP_GET;
        captive.handler = HttpCaptive;
        captive.user_ctx = this;
        httpd_register_uri_handler(httpd_, &captive);
    }

    provision_outcome_.store(ProvisionOutcome::kWaiting);
    provision_ssid_[0] = '\0';
    provision_password_[0] = '\0';
    if (events_ != nullptr) {
        xEventGroupClearBits(events_, kProvisionedBit);
    }
    provisioning_ = true;
    if (info != nullptr) {
        *info = softap_;
    }
    ESP_LOGI(kTag, "SoftAP %s ready at %s", softap_.ssid, softap_.url);
    return ESP_OK;
}

ZectrixWifi::ProvisionOutcome ZectrixWifi::PollProvisioning() {
    if (!provisioning_) {
        const auto outcome = provision_outcome_.load();
        return outcome == ProvisionOutcome::kIdle ? ProvisionOutcome::kFail
                                                  : outcome;
    }
    if (provision_outcome_.load() == ProvisionOutcome::kSuccess) {
        return ProvisionOutcome::kSuccess;
    }
    if (events_ != nullptr) {
        const EventBits_t bits = xEventGroupWaitBits(
            events_, kProvisionedBit, pdTRUE, pdFALSE, 0);
        if ((bits & kProvisionedBit) != 0) {
            provision_outcome_.store(ProvisionOutcome::kSuccess);
        }
    }
    return provision_outcome_.load();
}

esp_err_t ZectrixWifi::ApplyProvisionedProfile(size_t* out_index) {
    if (provision_outcome_.load() != ProvisionOutcome::kSuccess ||
        provision_ssid_[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    return UpsertProfile(provision_ssid_, provision_password_, out_index);
}

esp_err_t ZectrixWifi::StopProvisioning() {
    if (httpd_ != nullptr) {
        httpd_stop(httpd_);
        httpd_ = nullptr;
    }
    if (provisioning_ &&
        provision_outcome_.load() == ProvisionOutcome::kWaiting) {
        provision_outcome_.store(ProvisionOutcome::kCancelled);
    }
    provisioning_ = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    return ESP_OK;
}

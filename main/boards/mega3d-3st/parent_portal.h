// Parent mode on the device: pauses home Wi-Fi, runs a WPA2 hotspot
// "Choco-XXXX" and serves kid::ParentApi on http://192.168.4.1.
#pragma once

#include <esp_http_server.h>
#include <esp_netif.h>

#include <functional>
#include <memory>
#include <string>

#include "kid/parent_api.h"
#include "kid/parent_store.h"
#include "kid/platform.h"

class ParentPortal {
public:
    explicit ParentPortal(kid::Platform& platform);

    // Called from the LVGL task. on_stopped runs (from another task) once home
    // Wi-Fi has been restarted.
    bool Start(kid::Platform::ParentPortalInfo& out);
    void Stop(std::function<void()> on_stopped);
    bool active() const { return active_; }

private:
    kid::Platform& platform_;
    kid::ui::UiParentStore store_;
    kid::ParentApi api_;
    httpd_handle_t server_ = nullptr;
    esp_netif_t* ap_netif_ = nullptr;
    bool active_ = false;

    std::string Password();
    static esp_err_t HandleRequest(httpd_req_t* req);
};

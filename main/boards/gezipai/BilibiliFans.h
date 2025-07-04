#ifndef BILIIBILIFANS_H
#define BILIBIILIFANS_H

#include "board.h"
#include <esp_log.h>
#include <cJSON.h>
#include "esp_event.h"
#include "esp_wifi.h"


class BilibiliFans 
{
private:
    std::string uid_ = "396355825";     //396355825
        int follower_count_ = -1;
    bool refresh_in_progress_ = false;
    esp_timer_handle_t clock_timer_handle_ = nullptr;

    // int follower_count_ = 0;

    bool wifi_connected_ = false; // 标记 Wi-Fi 状态

    bool RefreshFollowerCount();

    static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

    void OnWifiConnected();

    void OnWifiDisconnected();

    void UpdateFansCount();

    void InitializeEvent();


public:
 
    ~BilibiliFans()
    {
        if (clock_timer_handle_)
        {
            esp_timer_stop(clock_timer_handle_);
            esp_timer_delete(clock_timer_handle_);
            clock_timer_handle_ = nullptr;
        }

        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &BilibiliFans::wifi_event_handler);
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &BilibiliFans::wifi_event_handler);
    }

    explicit BilibiliFans();
};

#endif // LED_STRIP_CONTROL_H

#include "BilibiliFans.h"
#include "settings.h"
#include "mcp_server.h"
#include <esp_log.h>
#include <memory>
#include <http.h>

#define TAG "BilibiliFans"

bool BilibiliFans::RefreshFollowerCount()
{
    if (uid_.empty())
    {
        ESP_LOGE(TAG, "UID is not set");
        return false;
    }

    if (refresh_in_progress_)
    {
        ESP_LOGI(TAG, "Refresh already in progress");
        return false;
    }

    refresh_in_progress_ = true;

    auto &board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = std::unique_ptr<Http>(network->CreateHttp(3));

    std::string url = "https://api.bilibili.com/x/relation/stat?vmid=" + uid_;
    http->SetHeader("User-Agent", "Mozilla/5.0");

    if (!http->Open("GET", url))
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        refresh_in_progress_ = false;
        return false;
    }

    std::string response = http->ReadAll();

    cJSON *root = cJSON_Parse(response.c_str());
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        refresh_in_progress_ = false;
        return false;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code == NULL || code->valueint != 0)
    {
        ESP_LOGE(TAG, "API returned error code");
        cJSON_Delete(root);
        refresh_in_progress_ = false;
        return false;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data == NULL)
    {
        ESP_LOGE(TAG, "No data field in response");
        cJSON_Delete(root);
        refresh_in_progress_ = false;
        return false;
    }

    cJSON *follower = cJSON_GetObjectItem(data, "follower");
    if (follower == NULL)
    {
        ESP_LOGE(TAG, "No follower field in data");
        cJSON_Delete(root);
        refresh_in_progress_ = false;
        return false;
    }

    follower_count_ = follower->valueint;
    ESP_LOGI(TAG, "B站用户 %s 的粉丝数：%d", uid_.c_str(), follower_count_);

    cJSON_Delete(root);
    refresh_in_progress_ = false;
    return true;
}

void BilibiliFans::wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    BilibiliFans *instance = static_cast<BilibiliFans *>(arg);

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ESP_LOGI("BiliFans", "Wi-Fi connected, updating fans count...");
        instance->OnWifiConnected();
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW("BiliFans", "Wi-Fi disconnected, pausing update timer...");
        instance->OnWifiDisconnected();
    }
}

void BilibiliFans::OnWifiConnected()
{
    wifi_connected_ = true;
    UpdateFansCount();
    if (clock_timer_handle_)
    {
        esp_timer_start_periodic(clock_timer_handle_, 1 * 60 * 1000000);
    }
}

void BilibiliFans::OnWifiDisconnected()
{
    wifi_connected_ = false;
    if (clock_timer_handle_)
    {
        esp_timer_stop(clock_timer_handle_);
    }
}

void BilibiliFans::UpdateFansCount()
{
    RefreshFollowerCount();
    if (follower_count_ < 0)
    {
        ESP_LOGE(TAG, "Failed to get fans count");
    }
    ESP_LOGI(TAG, "Fans count updated: %d", follower_count_);
}
void BilibiliFans::InitializeEvent()
{
    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void *arg)
        {
            BilibiliFans *instance = (BilibiliFans *)(arg);
            instance->UpdateFansCount();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "BiliFansUpdateTimer",
        .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &BilibiliFans::wifi_event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &BilibiliFans::wifi_event_handler, this));
}

BilibiliFans::BilibiliFans()
{
    // 从设置中读取亮度等级
    InitializeEvent();
    auto &mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.bilibilifans.uid",
                       "B站用户ID",
                       PropertyList(), [this](const PropertyList &properties) -> ReturnValue
                       { return uid_; });
    mcp_server.AddTool("self.bilibilifans.set_uid",
                       "设置B站用户ID",
                       PropertyList({Property("uid", kPropertyTypeString)}), [this](const PropertyList &properties) -> ReturnValue
                       {
            uid_ = properties["uid"].value<std::string>();
            ESP_LOGI(TAG, "设置B站用户ID为: %s", uid_.c_str());
            return true; });
    mcp_server.AddTool("self.bilibilifans.report_fansCount",
                       "汇报粉丝数量",
                       PropertyList(), [this](const PropertyList &properties) -> ReturnValue
                       { return follower_count_; });

    mcp_server.AddTool("self.bilibilifans.flash_fansCount",
                       "刷新粉丝数量",
                       PropertyList(), [this](const PropertyList &properties) -> ReturnValue
                       {
        ESP_LOGI(TAG, "刷新B站粉丝数量");
        UpdateFansCount();
        return true; });
}


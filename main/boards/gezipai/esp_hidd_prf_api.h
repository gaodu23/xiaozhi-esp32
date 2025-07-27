#ifndef __ESP_HIDD_PRI_API_H__
#define __ESP_HIDD_PRI_API_H__

#include "esp_bt_defs.h"
#include "esp_gatt_defs.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BLE HIDD callback events type
 */
typedef enum {
    ESP_HIDD_EVENT_REG_FINISH = 0,
    ESP_BAT_EVENT_REG,
    ESP_HIDD_EVENT_DEINIT_FINISH,
    ESP_HIDD_EVENT_BLE_CONNECT,
    ESP_HIDD_EVENT_BLE_DISCONNECT,
    ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT,
    ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT,
} esp_hidd_cb_event_t;

/**
 * @brief BLE HIDD callback parameters union
 */
typedef union {
    /**
     * @brief ESP_HIDD_EVENT_INIT_FINISH
     */
    struct hidd_init_finish_evt_param {
        esp_gatt_status_t state;				/*!< Initial status */
        esp_gatt_if_t gatts_if;
    } init_finish;							    /*!< Initial finish event parameters */

    /**
     * @brief ESP_HIDD_EVENT_DEINIT_FINISH
     */
    struct hidd_deinit_finish_evt_param {
        esp_gatt_status_t state;				/*!< De-initial status */
    } deinit_finish;						    /*!< De-initial finish event parameters */

    /**
     * @brief ESP_HIDD_EVENT_CONNECT
     */
    struct hidd_connect_evt_param {
        uint16_t conn_id;
        esp_bd_addr_t remote_bda;                   /*!< HID Remote bluetooth device address */
    } connect;							        /*!< HID Connect event parameters */

    /**
     * @brief ESP_HIDD_EVENT_DISCONNECT
     */
    struct hidd_disconnect_evt_param {
        esp_bd_addr_t remote_bda;                   /*!< HID Remote bluetooth device address */
    } disconnect;							    /*!< HID Disconnect event parameters */

    /**
     * @brief ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT
     */
    struct hidd_vendor_write_evt_param {
        uint16_t conn_id;                           /*!< HID connection index */
        uint16_t report_id;                         /*!< HID report index */
        uint16_t length;                            /*!< data length */
        uint8_t  *data;                             /*!< The pointer to the data */
    } vendor_write;						        /*!< Vendor write event parameters */

    /**
     * @brief ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT
     */
    struct hidd_led_write_evt_param {
        uint16_t conn_id;
        uint8_t report_id;
        uint8_t length;
        uint8_t *data;
    } led_write;
} esp_hidd_cb_param_t;

/**
 * @brief HID device callback function type
 * @param event : Event type
 * @param param : Point to callback parameter, currently is union type
 */
typedef void (*esp_hidd_event_cb_t) (esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param);

/**
 * @brief This function is called to initialize HID device profile
 *
 * @return
 *      - ESP_OK: success
 *      - other: failed
 */
esp_err_t esp_ble_hidd_profile_init(void);

/**
 * @brief This function is called to de-initialize HID device profile
 *
 * @return
 *      - ESP_OK: success
 *      - other: failed
 */
esp_err_t esp_ble_hidd_profile_deinit(void);

/**
 * @brief Get hidd profile version
 *
 * @return Most 8bit significant is Great version, Least 8bit is Sub version
 */
uint16_t esp_hidd_get_version(void);

/**
 * @brief This function is called to register HID device callbacks
 *
 * @param[in]       callbacks: pointer to the application callbacks
 *
 * @return
 *      - ESP_OK: success
 *      - other: failed
 */
esp_err_t esp_ble_hidd_register_callbacks(esp_hidd_event_cb_t callbacks);

/**
 * @brief This function is called to send HID report
 *
 * @param[in] map_index: the report map index
 * @param[in] report_id: the report id
 * @param[in] report_type: the report type
 * @param[in] report_data: the report data
 * @param[in] report_size: the report size
 *
 * @return
 *      - ESP_OK: success
 *      - other: failed
 */
esp_err_t esp_hidd_send_report(uint8_t map_index, uint8_t report_id, 
                               uint8_t report_type, uint8_t *report_data, 
                               uint8_t report_size);

/**
 * @brief This function is called to send HID mouse value
 *
 * @param[in] conn_id: connection id
 * @param[in] mouse_button: mouse button status
 * @param[in] micX: mouse X movement
 * @param[in] micY: mouse Y movement
 *
 * @return
 *      - ESP_OK: success
 *      - other: failed
 */
void esp_hidd_send_mouse_value(uint16_t conn_id, uint8_t mouse_button, int8_t micX, int8_t micY);

/**
 * @brief This function is called to send consumer control value
 *
 * @param[in] conn_id: connection id
 * @param[in] key_cmd: consumer control key command
 * @param[in] key_pressed: key press status
 *
 * @return
 *      - ESP_OK: success
 *      - other: failed
 */
esp_err_t esp_hidd_send_consumer_value(uint16_t conn_id, uint8_t key_cmd, bool key_pressed);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_HIDD_PRI_API_H__ */

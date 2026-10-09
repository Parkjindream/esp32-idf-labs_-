#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/i2c_master.h"

#define WIFI_SSID           "Wokwi-GUEST"
#define WIFI_PASS           ""
#define MQTT_BROKER_URI     "mqtt://broker.hivemq.com:1883"
#define MQTT_TOPIC          "esp32/lab4/sensor"

#define I2C_MASTER_SCL_IO   22
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_FREQ_HZ  100000
#define LCD_ADDR            0x27

#define LCD_CLEARDISPLAY    0x01
#define LCD_RETURNHOME      0x02
#define LCD_ENTRYMODESET    0x04
#define LCD_DISPLAYCONTROL  0x08
#define LCD_CURSORSHIFT     0x10
#define LCD_FUNCTIONSET     0x20
#define LCD_SETDDRAMADDR    0x80

#define LCD_DISPLAYON       0x04
#define LCD_2LINE           0x08
#define LCD_5x8DOTS         0x00
#define LCD_BACKLIGHT       0x08
#define LCD_ENABLE_BIT      0x04
#define LCD_RS_BIT          0x01

static const char *TAG = "MODULE_4_MQTT";

typedef struct {
    float temperature;
    float humidity;
    uint32_t count;
} sensor_data_t;

static QueueHandle_t xDisplayQueue = NULL;
static QueueHandle_t xMqttQueue = NULL;
static i2c_master_dev_handle_t lcd_dev_handle = NULL;
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool is_mqtt_connected = false;

// --- ฟังก์ชันควบคุม LCD1602 via I2C ---
static esp_err_t lcd_send_nibble(uint8_t nibble, uint8_t mode) {
    uint8_t data = (nibble & 0xF0) | mode | LCD_BACKLIGHT;
    uint8_t buf[2] = { data | LCD_ENABLE_BIT, data & ~LCD_ENABLE_BIT };
    return i2c_master_transmit(lcd_dev_handle, buf, 2, 1000);
}

static esp_err_t lcd_send_byte(uint8_t byte, uint8_t mode) {
    esp_err_t err = lcd_send_nibble(byte & 0xF0, mode);
    if (err != ESP_OK) return err;
    return lcd_send_nibble((byte << 4) & 0xF0, mode);
}

static void lcd_send_cmd(uint8_t cmd) {
    lcd_send_byte(cmd, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
}

static void lcd_send_char(char data) {
    lcd_send_byte(data, LCD_RS_BIT);
    vTaskDelay(pdMS_TO_TICKS(1));
}

static void lcd_clear(void) {
    lcd_send_cmd(LCD_CLEARDISPLAY);
    vTaskDelay(pdMS_TO_TICKS(2));
}

static void lcd_set_cursor(uint8_t col, uint8_t row) {
    uint8_t row_offsets[] = {0x00, 0x40};
    lcd_send_cmd(LCD_SETDDRAMADDR | (col + row_offsets[row]));
}

static void lcd_put_string(const char *str) {
    while (*str) {
        lcd_send_char(*str++);
    }
}

static void lcd_init(void) {
    vTaskDelay(pdMS_TO_TICKS(50));
    lcd_send_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    lcd_send_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_nibble(0x30, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    lcd_send_nibble(0x20, 0);
    vTaskDelay(pdMS_TO_TICKS(1));

    lcd_send_cmd(LCD_FUNCTIONSET | LCD_2LINE | LCD_5x8DOTS);
    lcd_send_cmd(LCD_DISPLAYCONTROL | LCD_DISPLAYON);
    lcd_clear();
}

// --- Event Handlers ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Wi-Fi Connected! Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (mqtt_client) {
            esp_mqtt_client_start(mqtt_client);
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected to Broker!");
            is_mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected from Broker!");
            is_mqtt_connected = false;
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT Packet Published Successfully (msg_id=%d)", event->msg_id);
            break;
        default:
            break;
    }
}

// --- Tasks ---
void sensor_task(void *pvParameters) {
    sensor_data_t data;
    uint32_t counter = 1;

    while (1) {
        data.temperature = 25.0f + (counter % 5) * 0.5f;
        data.humidity = 60.0f + (counter % 4) * 1.2f;
        data.count = counter;

        ESP_LOGI(TAG, "[SENSOR_TASK] Read data #%lu: Temp=%.1fC, Hum=%.1f%%",
                 (unsigned long)counter, data.temperature, data.humidity);

        xQueueSend(xDisplayQueue, &data, pdMS_TO_TICKS(100));
        xQueueSend(xMqttQueue, &data, pdMS_TO_TICKS(100));

        counter++;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void display_task(void *pvParameters) {
    sensor_data_t rx_data;
    char line1_buf[17];
    char line2_buf[17];

    while (1) {
        if (xQueueReceive(xDisplayQueue, &rx_data, portMAX_DELAY) == pdTRUE) {
            snprintf(line1_buf, sizeof(line1_buf), "T:%.1fC MQTT:%s", 
                     rx_data.temperature, is_mqtt_connected ? "OK" : "..");
            snprintf(line2_buf, sizeof(line2_buf), "H:%.1f%% #%lu", 
                     rx_data.humidity, (unsigned long)rx_data.count);

            lcd_clear();
            lcd_set_cursor(0, 0);
            lcd_put_string(line1_buf);
            lcd_set_cursor(0, 1);
            lcd_put_string(line2_buf);
        }
    }
}

void mqtt_task(void *pvParameters) {
    sensor_data_t mqtt_data;
    char payload[128];

    while (1) {
        if (xQueueReceive(xMqttQueue, &mqtt_data, portMAX_DELAY) == pdTRUE) {
            if (is_mqtt_connected) {
                snprintf(payload, sizeof(payload), 
                         "{\"count\":%lu,\"temp\":%.1f,\"hum\":%.1f}", 
                         (unsigned long)mqtt_data.count, mqtt_data.temperature, mqtt_data.humidity);

                int msg_id = esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, payload, 0, 1, 0);
                ESP_LOGI(TAG, "[MQTT_TASK] Published JSON: %s (msg_id=%d)", payload, msg_id);
            } else {
                ESP_LOGW(TAG, "[MQTT_TASK] Waiting for MQTT connection before publishing...");
            }
        }
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LCD_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &lcd_dev_handle));
    lcd_init();

    xDisplayQueue = xQueueCreate(5, sizeof(sensor_data_t));
    xMqttQueue = xQueueCreate(5, sizeof(sensor_data_t));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = MQTT_BROKER_URI,
            },
        },
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    xTaskCreate(sensor_task, "sensor_task", 3072, NULL, 5, NULL);
    xTaskCreate(display_task, "display_task", 3072, NULL, 5, NULL);
    xTaskCreate(mqtt_task, "mqtt_task", 3072, NULL, 5, NULL);
}
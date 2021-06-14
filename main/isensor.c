/* Power save Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
   this example shows how to use power save mode
   set a router or a AP using the same SSID&PASSWORD as configuration of this example.
   start esp32 and when it connected to AP it will enter power save mode
*/
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "esp_sleep.h"

#include "driver/rtc_io.h"
#include "soc/rtc.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "mqtt_client.h"

#include "driver/i2c.h"
#include "bh1750.h"
#include "htu21d.h"

#include "ili9340.h"
//#include "fontx.h"
// #include "bmpfile.h"
// #include "decode_image.h"
// #include "pngle.h"

/*set the ssid and password via "idf.py menuconfig"*/
#define DEFAULT_SSID CONFIG_EXAMPLE_WIFI_SSID
#define DEFAULT_PWD CONFIG_EXAMPLE_WIFI_PASSWORD

static const char *TAG = "isensor";

static EventGroupHandle_t event_group;
const int SENSOR_READ = BIT0;
const int WIFI_CONNECTED = BIT1;
const int MQTT_PUBLISHED = BIT2;

// FreeRTOS
#define DELAY_TIME_BETWEEN_ITEMS_MS 300000 /*!< delay time between different test items */
#define ENV_DATA_QUEUE_SIZE 10

static xQueueHandle env_data_queue;
SemaphoreHandle_t read_sensor_mux = NULL;

typedef struct {
	float temperature;
	float humidity;
    //float lux;
} env_data_t;

#define _I2C_NUMBER(num) I2C_NUM_##num
#define I2C_NUMBER(num) _I2C_NUMBER(num)

#define I2C_MASTER_SCL_IO 19            /*!< gpio number for I2C master clock */
#define I2C_MASTER_SDA_IO 18            /*!< gpio number for I2C master data  */
#define I2C_MASTER_NUM 0                /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ 100000       /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE 0     /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0     /*!< I2C master doesn't need buffer */

/**
 * @brief i2c master initialization
 */
static esp_err_t i2c_master_init(void)
{
    int i2c_master_port = I2C_MASTER_NUM;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
        // .clk_flags = 0,          /*!< Optional, you can use I2C_SCLK_SRC_FLAG_* flags to choose i2c source clock here. */
    };
    esp_err_t err = i2c_param_config(i2c_master_port, &conf);
    if (err != ESP_OK)
    {
        return err;
    }
    return i2c_driver_install(i2c_master_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

/**
 * @brief BH1750 sensor
 *
 */
static void i2c_read_bh1750(void *arg)
{   
    int ret;
    
    while(1) {
        xSemaphoreTake(read_sensor_mux, portMAX_DELAY);
        ret = bh1750_init(I2C_MASTER_NUM);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "Light: %.02f[Lux]\n", bh1750_read_lux());
        }
        else
        {
            ESP_LOGW(TAG, "%s: No ack, sensor not connected...skip...", esp_err_to_name(ret));
        }
        xSemaphoreGive(read_sensor_mux);
        vTaskDelay(DELAY_TIME_BETWEEN_ITEMS_MS / portTICK_RATE_MS);
    }
    vSemaphoreDelete(read_sensor_mux);
    vTaskDelete(NULL);
}

/**
 * @brief HTU21D sensor
 *
 */
static void i2c_read_htu21d(void *arg)
{
    int ret;
    env_data_t env_data;

    while(1) {
        xSemaphoreTake(read_sensor_mux, portMAX_DELAY);
        ret = htu21d_init(I2C_MASTER_NUM);
        if (ret == ESP_OK)
        {
            env_data.temperature = ht21d_read_temperature();
            env_data.humidity = ht21d_read_humidity();
            ESP_LOGI(TAG, "Temperature %.2f[C], Humidity %.2f[%%]", env_data.temperature, env_data.humidity);

            xQueueSend(env_data_queue, &env_data, portMAX_DELAY);
        }
        else
        {
            ESP_LOGW(TAG, "%s: No ack, sensor not connected...skip...", esp_err_to_name(ret));
        }
        xSemaphoreGive(read_sensor_mux);
        xEventGroupSetBits(event_group, SENSOR_READ);
        vTaskDelay(DELAY_TIME_BETWEEN_ITEMS_MS / portTICK_RATE_MS);
    }
    vSemaphoreDelete(read_sensor_mux);
    vTaskDelete(NULL);
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        char data[256];
        env_data_t env_data;

        xQueueReceive(env_data_queue, &env_data, portMAX_DELAY);
        sprintf(data, "{\"temperature\": %.2f, \"humidity\": %.2f}", env_data.temperature, env_data.humidity);
        esp_mqtt_client_publish(client, "sensors/iSensor01", data, 0, 0, 0);
        vTaskDelay(100/portTICK_RATE_MS);
        //esp_mqtt_client_disconnect(client);
        xEventGroupSetBits(event_group, MQTT_PUBLISHED);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void *arg)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://192.168.10.99",
    };

    while(1) {
        xEventGroupWaitBits(event_group, WIFI_CONNECTED, true, true, portMAX_DELAY);

        esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
        /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        esp_mqtt_client_start(client);

        vTaskDelay(DELAY_TIME_BETWEEN_ITEMS_MS / portTICK_RATE_MS);
    }

    vTaskDelete(NULL);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(event_group, WIFI_CONNECTED);
    }
}

/*init wifi as sta and set power save mode*/
static void wifi_power_save()
{
    while(1) {
        xEventGroupWaitBits(event_group, SENSOR_READ, true, true, portMAX_DELAY); 

        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
        assert(sta_netif);

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

        wifi_config_t wifi_config = {
            .sta = {
                .ssid = DEFAULT_SSID,
                .password = DEFAULT_PWD,
                .listen_interval = 3,
                // .scan_method = WIFI_FAST_SCAN,
                // .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
                // .threshold.rssi = -127,
                // .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
            },
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "esp_wifi_set_ps().");
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

        vTaskDelay(100 / portMAX_DELAY);
    }
}

void esp_pm_config()
{
#if CONFIG_PM_ENABLE
    // Configure dynamic frequency scaling:
    // maximum and minimum frequencies are set in sdkconfig,
    // automatic light sleep is enabled if tickless idle support is enabled.
    #if CONFIG_IDF_TARGET_ESP32
        esp_pm_config_esp32_t pm_config = {
    #elif CONFIG_IDF_TARGET_ESP32S2
        esp_pm_config_esp32s2_t pm_config = {
    #elif CONFIG_IDF_TARGET_ESP32C3
        esp_pm_config_esp32c3_t pm_config = {
    #endif
                .max_freq_mhz = CONFIG_EXAMPLE_MAX_CPU_FREQ_MHZ,
                .min_freq_mhz = CONFIG_EXAMPLE_MIN_CPU_FREQ_MHZ,
    #if CONFIG_FREERTOS_USE_TICKLESS_IDLE
                .light_sleep_enable = true
    #endif
        };
        ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );
#endif // CONFIG_PM_ENABLE
}

static void go_to_deep_sleep(void *arg)
{
    while(1)
    {   
        xEventGroupWaitBits(event_group, MQTT_PUBLISHED, true, true, portMAX_DELAY);
        printf("Entering deep sleep\n");
        // stop wifi before going to deep sleep
        esp_wifi_stop();
        //gettimeofday(&sleep_enter_time, NULL);
        esp_deep_sleep_start();
    }
}

TickType_t ColorBarTest(TFT_t * dev, int width, int height) {
	TickType_t startTick, endTick, diffTick;
	startTick = xTaskGetTickCount();

	if (width < height) {
		uint16_t y1,y2;
		y1 = height/3;
		y2 = (height/3)*2;
		lcdDrawFillRect(dev, 0, 0, width-1, y1-1, RED);
		vTaskDelay(1);
		lcdDrawFillRect(dev, 0, y1-1, width-1, y2-1, GREEN);
		vTaskDelay(1);
		lcdDrawFillRect(dev, 0, y2-1, width-1, height-1, BLUE);
	} else {
		uint16_t x1,x2;
		x1 = width/3;
		x2 = (width/3)*2;
		lcdDrawFillRect(dev, 0, 0, x1-1, height-1, RED);
		vTaskDelay(1);
		lcdDrawFillRect(dev, x1-1, 0, x2-1, height-1, GREEN);
		vTaskDelay(1);
		lcdDrawFillRect(dev, x2-1, 0, width-1, height-1, BLUE);
	}

	endTick = xTaskGetTickCount();
	diffTick = endTick - startTick;
	ESP_LOGI(__FUNCTION__, "elapsed time[ms]:%d",diffTick*portTICK_RATE_MS);
	return diffTick;
}

void ILI9341(void *pvParameters)
{
	TFT_t dev;
	spi_master_init(&dev, 25, 23, 14, 27, 26, 12);
	uint16_t model = 0x7735;
	lcdInit(&dev, model, 80, 160, 26, 1);

	ESP_LOGI(TAG, "Enable Display Inversion");
	lcdInversionOn(&dev);

	while(1) {

		// lcdDisplayOff(&dev);
		// lcdSleepIn(&dev);
		// ESP_LOGI(TAG, "lcdSleepIn");
		// vTaskDelay(400 / portTICK_PERIOD_MS);
		
		// lcdSleepOut(&dev);
        // vTaskDelay(400 / portTICK_PERIOD_MS);
		// lcdDisplayOn(&dev);
		// ESP_LOGI(TAG, "lcdSleepOut");

		ColorBarTest(&dev, 80, 160);
		vTaskDelay(DELAY_TIME_BETWEEN_ITEMS_MS / portTICK_PERIOD_MS);

	} // end while
}

void app_main(void)
{
    // uint64_t start, end;
    // start = esp_timer_get_time();

    // Wake-up timer
    esp_sleep_enable_timer_wakeup(DELAY_TIME_BETWEEN_ITEMS_MS * 1000);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );
    
    // pm management
    esp_pm_config();

    // RTOS Event Group
    event_group = xEventGroupCreate();

    // Data Queue
    env_data_queue = xQueueCreate(ENV_DATA_QUEUE_SIZE, sizeof(env_data_t));
    if (env_data_queue == NULL)
    {
    	ESP_LOGE(TAG, "Failed to create env_data_queue.");
    }

    // Semaphore Mutex
    read_sensor_mux = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(i2c_master_init());

    xTaskCreate(i2c_read_bh1750,"i2c_read_bh1750", 1024 * 2, (void *)0, 10, NULL);
    xTaskCreate(i2c_read_htu21d,"i2c_read_htu21d", 1024 * 2, (void *)0, 10, NULL);
    xTaskCreate(wifi_power_save, "wifi_power_save", 1024 * 4, (void *)0, 10, NULL);
    xTaskCreate(mqtt_app_start, "mqtt_app_start", 1024 * 4, (void *)0, 10, NULL);
    xTaskCreate(go_to_deep_sleep,"go_to_deep_sleep", 1024 * 2, (void *)0, 10, NULL);
    xTaskCreate(ILI9341, "ILI9341", 1024*6, NULL, 2, NULL);

    // end = esp_timer_get_time();
    // printf("took %llu(ms)\n", (end-start)/1000);
}

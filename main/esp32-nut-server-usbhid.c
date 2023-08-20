/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "errno.h"
#include "driver/gpio.h"

#include "usb/hid_host.h"

#include "driver/gptimer.h"
#include "freertos/queue.h"

#include "sys/socket.h"
#include "netdb.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "led_strip.h"

#include "cJSON.h"

/* It is use for change beep status */
#define APP_QUIT_PIN GPIO_NUM_0
#define RGB_LED_PIN GPIO_NUM_48

static led_strip_handle_t led_strip;

static const char *TAG = "ups";
QueueHandle_t hid_host_event_queue;
QueueHandle_t timer_queue;
typedef struct
{
    uint64_t event_count;
} timer_queue_element_t;
bool user_shutdown = false;
bool UPS_DEV_CONNECTED = false;
hid_host_device_handle_t latest_hid_device_handle;
/**
 * @brief HID Host event
 *
 * This event is used for delivering the HID Host event from callback to a task.
 */
typedef struct
{
    hid_host_device_handle_t hid_device_handle;
    hid_host_driver_event_t event;
    void *arg;
} hid_host_event_queue_t;

/**
 * @brief HID Protocol string names
 */
static const char *hid_proto_name_str[] = {
    "NONE",
    "KEYBOARD",
    "MOUSE"};

// =tcp server

static char *ori_json = "{\"battery\":{\"charge\":{\"_root\":\"100\",\"low\":\"20\"},\"charger\":{\"status\":\"charging\"},\"runtime\":\"1104\",\"type\":\"PbAc\"},\"device\":{\"mfr\":\"EATON\",\"model\":\"SANTAK TG-BOX 850\",\"serial\":\"Blank\",\"type\":\"ups\"},\"driver\":{\"name\":\"usbhid-ups\",\"parameter\":{\"pollfreq\":30,\"pollinterval\":2,\"port\":\"/dev/ttyS1\",\"synchronous\":\"no\"},\"version\":{\"_root\":\"2.7.4\",\"data\":\"MGE HID 1.39\",\"internal\":\"0.41\"}},\"input\":{\"transfer\":{\"high\":\"264\",\"low\":\"184\"}},\"outlet\":{\"1\":{\"desc\":\"PowerShare Outlet 1\",\"id\":\"1\",\"status\":\"on\",\"switchable\":\"no\"},\"desc\":\"Main Outlet\",\"id\":\"0\",\"switchable\":\"yes\"},\"output\":{\"frequency\":{\"nominal\":\"50\"},\"voltage\":{\"_root\":\"230.0\",\"nominal\":\"220\"}},\"ups\":{\"beeper\":{\"status\":\"enabled\"},\"delay\":{\"shutdown\":\"20\",\"start\":\"30\"},\"firmware\":\"02.08.0010\",\"load\":\"28\",\"mfr\":\"EATON\",\"model\":\"SANTAK TG-BOX 850\",\"power\":{\"nominal\":\"850\"},\"productid\":\"ffff\",\"serial\":\"Blank\",\"status\":\"OL\",\"timer\":{\"shutdown\":\"0\",\"start\":\"0\"},\"type\":\"offline / line interactive\",\"vendorid\":\"0463\"}}";
cJSON *json_object;

char nut_list_var_text[2048]="";

void init_json_object()
{
    json_object = cJSON_Parse(ori_json);
}

void gen_nut_list_var_text(cJSON *input, char *parent_path)
{
    char *prefix_text = "VAR qnapups ";
    if (cJSON_IsString(input))
    {
        strcat(nut_list_var_text, parent_path);
        if (input->string[0] != '_')
        {
            strcat(nut_list_var_text, ".");
            strcat(nut_list_var_text, input->string);
        }
        strcat(nut_list_var_text, " \"");
        strcat(nut_list_var_text, input->valuestring);
        strcat(nut_list_var_text, "\"\n");
    }
    else if(cJSON_IsObject(input))
    {
        char new_parent_path[64] = "";
        strcpy(new_parent_path, parent_path);
        if (input->string != NULL)
        {
            if (strlen(new_parent_path) > strlen(prefix_text))
            {
                strcat(new_parent_path, ".");
            }
            strcat(new_parent_path, input->string);
        }
        else
        {
            strcat(new_parent_path, prefix_text);
        }
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, input)
        {
            gen_nut_list_var_text(child, new_parent_path);
        }
    }
}

/// @brief https://networkupstools.org/docs/developer-guide.chunked/ar01s09.html
void gen_nut_list_var_text_wrapper()
{
    strcpy(nut_list_var_text, "BEGIN LIST VAR qnapups\n");
    gen_nut_list_var_text(json_object, "");
    strcat(nut_list_var_text, "END LIST VAR qnapups\n");
}

bool str_startswith(const char *str, const char *p)
{
	int len = strlen(p);
	if (len <= 0)
		return 0;
	if (strncmp(str, p, len) == 0)
		return 1;

	return 0;
}

/**
 * @brief Indicates that the file descriptor represents an invalid (uninitialized or closed) socket
 *
 * Used in the TCP server structure `sock[]` which holds list of active clients we serve.
 */
#define INVALID_SOCK (-1)

/**
 * @brief Time in ms to yield to all tasks when a non-blocking socket would block
 *
 * Non-blocking socket operations are typically executed in a separate task validating
 * the socket status. Whenever the socket returns `EAGAIN` (idle status, i.e. would block)
 * we have to yield to all tasks to prevent lower priority tasks from starving.
 */
#define YIELD_TO_ALL_MS 50

/**
 * @brief Utility to log socket errors
 *
 * @param[in] tag Logging tag
 * @param[in] sock Socket number
 * @param[in] err Socket errno
 * @param[in] message Message to print
 */
static void log_socket_error(const char *tag, const int sock, const int err, const char *message)
{
    ESP_LOGE(tag, "[sock=%d]: %s\n"
                  "error=%d: %s", sock, message, err, strerror(err));
}

/**
 * @brief Tries to receive data from specified sockets in a non-blocking way,
 *        i.e. returns immediately if no data.
 *
 * @param[in] tag Logging tag
 * @param[in] sock Socket for reception
 * @param[out] data Data pointer to write the received data
 * @param[in] max_len Maximum size of the allocated space for receiving data
 * @return
 *          >0 : Size of received data
 *          =0 : No data available
 *          -1 : Error occurred during socket read operation
 *          -2 : Socket is not connected, to distinguish between an actual socket error and active disconnection
 */
static int try_receive(const char *tag, const int sock, char * data, size_t max_len)
{
    int len = recv(sock, data, max_len, 0);
    if (len < 0) {
        if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;   // Not an error
        }
        if (errno == ENOTCONN) {
            ESP_LOGW(tag, "[sock=%d]: Connection closed", sock);
            return -2;  // Socket has been disconnected
        }
        if (errno == ECONNRESET)
        {
            //will happen as nut design. Not an error when communicating with nut clients
            return 0;
        }
        
        log_socket_error(tag, sock, errno, "Error occurred during receiving");
        return -1;
    }

    return len;
}

/**
 * @brief Sends the specified data to the socket. This function blocks until all bytes got sent.
 *
 * @param[in] tag Logging tag
 * @param[in] sock Socket to write data
 * @param[in] data Data to be written
 * @param[in] len Length of the data
 * @return
 *          >0 : Size the written data
 *          -1 : Error occurred during socket write operation
 */
static int socket_send(const char *tag, const int sock, const char * data, const size_t len)
{
    int to_write = len;
    while (to_write > 0) {
        int written = send(sock, data + (len - to_write), to_write, 0);
        if (written < 0 && errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
            log_socket_error(tag, sock, errno, "Error occurred during sending");
            return -1;
        }
        to_write -= written;
    }
    return len;
}


/**
 * @brief Returns the string representation of client's address (accepted on this server)
 */
static inline char* get_clients_address(struct sockaddr_storage *source_addr)
{
    static char address_str[128];
    char *res = NULL;
    // Convert ip address to string
    if (source_addr->ss_family == PF_INET) {
        res = inet_ntoa_r(((struct sockaddr_in *)source_addr)->sin_addr, address_str, sizeof(address_str) - 1);
    }
#ifdef CONFIG_LWIP_IPV6
    else if (source_addr->ss_family == PF_INET6) {
        res = inet6_ntoa_r(((struct sockaddr_in6 *)source_addr)->sin6_addr, address_str, sizeof(address_str) - 1);
    }
#endif
    if (!res) {
        address_str[0] = '\0'; // Returns empty string if conversion didn't succeed
    }
    return address_str;
}

static void tcp_server_task(void *pvParameters)
{
    static char rx_buffer[128];
    static const char *TAG = "tcp-svr";
    SemaphoreHandle_t *server_ready = pvParameters;
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM };
    struct addrinfo *address_info;
    int listen_sock = INVALID_SOCK;
    const size_t max_socks = CONFIG_LWIP_MAX_SOCKETS - 1;
    static int sock[CONFIG_LWIP_MAX_SOCKETS - 1];

    // Prepare a list of file descriptors to hold client's sockets, mark all of them as invalid, i.e. available
    for (int i=0; i<max_socks; ++i) {
        sock[i] = INVALID_SOCK;
    }

    // Translating the hostname or a string representation of an IP to address_info
    int res = getaddrinfo(CONFIG_EXAMPLE_TCP_SERVER_BIND_ADDRESS, CONFIG_EXAMPLE_TCP_SERVER_BIND_PORT, &hints, &address_info);
    if (res != 0 || address_info == NULL) {
        ESP_LOGE(TAG, "couldn't get hostname for `%s` "
                      "getaddrinfo() returns %d, addrinfo=%p", CONFIG_EXAMPLE_TCP_SERVER_BIND_ADDRESS, res, address_info);
        goto error;
    }

    // Creating a listener socket
    listen_sock = socket(address_info->ai_family, address_info->ai_socktype, address_info->ai_protocol);

    if (listen_sock < 0) {
        log_socket_error(TAG, listen_sock, errno, "Unable to create socket");
        goto error;
    }
    ESP_LOGI(TAG, "Listener socket created");

    // Marking the socket as non-blocking
    int flags = fcntl(listen_sock, F_GETFL);
    if (fcntl(listen_sock, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_socket_error(TAG, listen_sock, errno, "Unable to set socket non blocking");
        goto error;
    }
    //ESP_LOGI(TAG, "Socket marked as non blocking");

    // Binding socket to the given address
    int err = bind(listen_sock, address_info->ai_addr, address_info->ai_addrlen);
    if (err != 0) {
        log_socket_error(TAG, listen_sock, errno, "Socket unable to bind");
        goto error;
    }
    ESP_LOGI(TAG, "Socket bound on %s:%s", CONFIG_EXAMPLE_TCP_SERVER_BIND_ADDRESS, CONFIG_EXAMPLE_TCP_SERVER_BIND_PORT);

    // Set queue (backlog) of pending connections to one (can be more)
    err = listen(listen_sock, 1);
    if (err != 0) {
        log_socket_error(TAG, listen_sock, errno, "Error occurred during listen");
        goto error;
    }
    ESP_LOGI(TAG, "Socket listening");
    xSemaphoreGive(*server_ready);

    // Main loop for accepting new connections and serving all connected clients
    while (1) {
        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t addr_len = sizeof(source_addr);

        // Find a free socket
        int new_sock_index = 0;
        for (new_sock_index=0; new_sock_index<max_socks; ++new_sock_index) {
            if (sock[new_sock_index] == INVALID_SOCK) {
                break;
            }
        }

        // We accept a new connection only if we have a free socket
        if (new_sock_index < max_socks) {
            // Try to accept a new connections
            sock[new_sock_index] = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);

            if (sock[new_sock_index] < 0) {
                if (errno == EWOULDBLOCK) { // The listener socket did not accepts any connection
                                            // continue to serve open connections and try to accept again upon the next iteration
                    ESP_LOGV(TAG, "No pending connections...");
                } else {
                    log_socket_error(TAG, listen_sock, errno, "Error when accepting connection");
                    goto error;
                }
            } else {
                // We have a new client connected -> print it's address
                ESP_LOGI(TAG, "[sock=%d]: Connection accepted from IP:%s", sock[new_sock_index], get_clients_address(&source_addr));

                led_strip_set_pixel(led_strip, 0, 0x01, 0x01, 0x01);
                /* Refresh the strip to send data */
                led_strip_refresh(led_strip);

                // ...and set the client's socket non-blocking
                flags = fcntl(sock[new_sock_index], F_GETFL);
                if (fcntl(sock[new_sock_index], F_SETFL, flags | O_NONBLOCK) == -1) {
                    log_socket_error(TAG, sock[new_sock_index], errno, "Unable to set socket non blocking");
                    goto error;
                }
                //ESP_LOGI(TAG, "[sock=%d]: Socket marked as non blocking", sock[new_sock_index]);
            }
        }

        // We serve all the connected clients in this loop
        for (int i=0; i<max_socks; ++i) {
            if (sock[i] != INVALID_SOCK) {

                // This is an open socket -> try to serve it
                int len = try_receive(TAG, sock[i], rx_buffer, sizeof(rx_buffer));
                if (len < 0) {
                    // Error occurred within this client's socket -> close and mark invalid
                    ESP_LOGI(TAG, "[sock=%d]: try_receive() returned %d -> closing the socket", sock[i], len);
                    close(sock[i]);
                    sock[i] = INVALID_SOCK;
                } else if (len > 0) {
                    if (len > 1 && rx_buffer[len-1] == '\n')
                    {
                        ESP_LOGI(TAG, "[sock=%d]: Received %.*s", sock[i], len-1, rx_buffer);
                    }
                    else
                    {
                        ESP_LOGI(TAG, "[sock=%d]: Received %.*s", sock[i], len, rx_buffer);
                    }
                    
                    char * rt;
                    char ok_text[4]="OK\n";
                    rt = ok_text;
                    
                    if (UPS_DEV_CONNECTED)
                    {
                        if (str_startswith(rx_buffer, "USERNAME") || str_startswith(rx_buffer, "PASSWORD") || str_startswith(rx_buffer, "LOGIN"))
                        {
                            // Safety Attention: will not check whether the user & password is correct.
                            // Make sure your LAN environment is safe.
                            rt = ok_text;
                        }
                        else if (str_startswith(rx_buffer, "LIST VAR"))
                        {
                            gen_nut_list_var_text_wrapper();
                            rt = nut_list_var_text;
                        }
                        else if (str_startswith(rx_buffer, "GET VAR qnapups ups.status"))
                        {
                            cJSON *got_item;
                            got_item = cJSON_GetObjectItemCaseSensitive(json_object, "ups");
                            got_item = cJSON_GetObjectItemCaseSensitive(got_item, "status");

                            char tmp_rt[30];
                            strcpy(tmp_rt, "VAR qnapups ups.status \"");
                            strcat(tmp_rt, cJSON_GetStringValue(got_item));
                            strcat(tmp_rt, "\"\n");
                            rt = tmp_rt;
                        }
                        else if (str_startswith(rx_buffer, "LOGOUT"))
                        {
                            char *bye_text = "OK Goodbye\n";
                            rt = bye_text;
                        }
                    }

                    int to_write = strlen(rt);
                    len = socket_send(TAG, sock[i], rt, to_write);
                    if (len < 0) {
                        // Error occurred on write to this socket -> close it and mark invalid
                        ESP_LOGI(TAG, "[sock=%d]: socket_send() returned %d -> closing the socket", sock[i], len);
                        close(sock[i]);
                        sock[i] = INVALID_SOCK;
                    } else {
                        // Successfully echoed to this socket
                        //ESP_LOGI(TAG, "[sock=%d]: Written %.*s", sock[i], len, rx_buffer);
                    }
                }

            } // one client's socket
        } // for all sockets

        // Yield to other tasks
        vTaskDelay(pdMS_TO_TICKS(YIELD_TO_ALL_MS));
    }

error:
    if (listen_sock != INVALID_SOCK) {
        close(listen_sock);
    }

    for (int i=0; i<max_socks; ++i) {
        if (sock[i] != INVALID_SOCK) {
            close(sock[i]);
        }
    }

    free(address_info);
    vTaskDelete(NULL);
}

// =tcp server

/**
 * @brief Makes new line depending on report output protocol type
 *
 * @param[in] proto Current protocol to output
 */
static void hid_print_new_device_report_header(hid_protocol_t proto)
{
    static hid_protocol_t prev_proto_output = -1;

    if (prev_proto_output != proto)
    {
        prev_proto_output = proto;
        printf("\r\n");
        if (proto == HID_PROTOCOL_MOUSE)
        {
            printf("Mouse\r\n");
        }
        else if (proto == HID_PROTOCOL_KEYBOARD)
        {
            printf("Keyboard\r\n");
        }
        else
        {
            printf("Generic\r\n");
        }
        fflush(stdout);
    }
}

/**
 * @brief USB HID Host Generic Interface report callback handler
 *
 * 'generic' means anything else than mouse or keyboard
 *
 * @param[in] data    Pointer to input report data buffer
 * @param[in] length  Length of input report data buffer
 */
static void hid_host_generic_report_callback(const uint8_t *const data, const int length)
{
    hid_print_new_device_report_header(HID_PROTOCOL_NONE);
    for (int i = 0; i < length; i++)
    {
        printf("%02X", data[i]);
    }
    putchar('\r');
}

/**
 * @brief USB HID Host interface callback
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host interface event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                 const hid_host_interface_event_t event,
                                 void *arg)
{
    uint8_t data[64] = {0};
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event)
    {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle,
                                                                  data,
                                                                  64,
                                                                  &data_length));

        if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class)
        {
        }
        else
        {
            hid_host_generic_report_callback(data, data_length);
        }

        break;
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        UPS_DEV_CONNECTED = false;
        ESP_LOGI(TAG, "hid_host_interface_callback: HID Device, protocol '%s' DISCONNECTED",
                 hid_proto_name_str[dev_params.proto]);
        ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGI(TAG, "hid_host_interface_callback: HID Device, protocol '%s' TRANSFER_ERROR",
                 hid_proto_name_str[dev_params.proto]);
        break;
    default:
        ESP_LOGE(TAG, "hid_host_interface_callback: HID Device, protocol '%s' Unhandled event",
                 hid_proto_name_str[dev_params.proto]);
        break;
    }
}

/**
 * @brief USB HID Host Device event
 *
 * @param[in] hid_device_handle  HID Device handle
 * @param[in] event              HID Host Device event
 * @param[in] arg                Pointer to arguments, does not used
 */
void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                           const hid_host_driver_event_t event,
                           void *arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event)
    {
    case HID_HOST_DRIVER_EVENT_CONNECTED:

        ESP_LOGI(TAG, "hid_host_device_event: HID Device, protocol '%s' CONNECTED",
                 hid_proto_name_str[dev_params.proto]);

        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL};

        ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
        if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class)
        {
            ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto)
            {
                ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
            }
        }
        ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
        latest_hid_device_handle = hid_device_handle;
        UPS_DEV_CONNECTED = true;
        break;
    default:
        break;
    }
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);

    while (true/*gpio_get_level(APP_QUIT_PIN) != 0*/)
    {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        // Release devices once all clients has deregistered
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            usb_host_device_free_all();
            ESP_LOGI(TAG, "USB Event flags: NO_CLIENTS");
        }
        // All devices were removed
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
        {
            ESP_LOGI(TAG, "USB Event flags: ALL_FREE");
        }
    }
    // App Button was pressed, trigger the flag
    user_shutdown = true;
    ESP_LOGI(TAG, "USB shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

void set_beep(bool enabled)
{
    uint8_t send[2] = {0x1f, 0x02};
    size_t len = 2;
    if (!enabled)
    {
        send[1] = 0x01;
    }
    
    hid_class_request_set_report(latest_hid_device_handle, 0x03, 0x1f, &send, len);
}

void refresh_ups_status_from_hid(bool *beep)
{
    /**
     * Everything below is specific for SANTAK TG-BOX 850
     * 
     * Those protocol is hard-encode here. It may not supported by other ups products.
     * 
     */
    uint8_t recv[8] = {0xff};
    size_t len;
    
    len = 4;
    memset(recv, 0xFF, len);
    hid_class_request_get_report(latest_hid_device_handle, 0x03, 0x01, &recv, &len);
    bool ac_present = recv[1] & 1;
    bool charging = recv[1] & (1 << 2);
    bool discharging = recv[1] & (1 << 4);
    bool good = recv[1] & (1 << 5);
    bool internal_failure = recv[1] & (1 << 6);
    bool need_replacement = recv[1] & (1 << 7);
    bool all_good_flag = good && !internal_failure && !need_replacement;
    bool overload = recv[2] > 0;
    bool shutdown_imminent = recv[3] > 0;

    char alert_text[20] = "";
    if (!all_good_flag)
    {
        strcat(alert_text, "【欠佳】");
    }
    if (overload)
    {
        strcat(alert_text, "【过载】");
    }
    if (shutdown_imminent)
    {
        strcat(alert_text, "【即将停供】");
    }

    len = 6;
    memset(recv, 0xFF, len);
    hid_class_request_get_report(latest_hid_device_handle, 0x03, 0x06, &recv, &len);
    size_t battery_charge = recv[1];
    size_t battery_runtime = recv[2] + 256 * recv[3] + 256 * 256 * recv[4] + 256 * 256 * 256 * recv[5];

    len = 8;
    memset(recv, 0xFF, len);
    hid_class_request_get_report(latest_hid_device_handle, 0x03, 0x07, &recv, &len);
    size_t ups_load = recv[6];

    len = 3;
    memset(recv, 0xFF, len);
    hid_class_request_get_report(latest_hid_device_handle, 0x03, 0x0e, &recv, &len);
    size_t actual_voltage = recv[1] + 256 * recv[2];

    len = 8;
    memset(recv, 0xFF, len);
    hid_class_request_get_report(latest_hid_device_handle, 0x03, 0x1f, &recv, &len);
    size_t audible_alarm_control = recv[1];

    /*
    * According to the UPS HID protocol,
    * AudibleAlarmControl:
    * 1: Disabled (Never sound)
    * 2: Enabled (Sound when an alarm is present)
    * 3: Muted (Temporarily silence the alarm)
    * I have not tested what will happen when flag = 3
    */
    *beep = audible_alarm_control == 2;

    // Start to change the json object text
    char setted_text[32];
    if (ac_present)
    {
        strcpy(setted_text, "OL");
    }
    else
    {
        strcpy(setted_text, "OB");
    }
    if (shutdown_imminent)
    {
        strcpy(setted_text, " LB");
    }
    if (!all_good_flag)
    {
        strcpy(setted_text, " RB");
    }
    if (overload)
    {
        strcpy(setted_text, " OVER");
    }
    cJSON *got_item;
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "ups");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "status");
    cJSON_SetValuestring(got_item, setted_text);

    itoa(battery_charge, setted_text, 10);
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "battery");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "charge");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "_root");
    cJSON_SetValuestring(got_item, setted_text);
    
    strcpy(setted_text, "");
    if (charging)
    {
        strcpy(setted_text, "charging");
    }
    else if (discharging)
    {
        strcpy(setted_text, "discharging");
    }
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "battery");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "charger");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "status");
    cJSON_SetValuestring(got_item, setted_text);

    itoa(battery_runtime, setted_text, 10);
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "battery");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "runtime");
    cJSON_SetValuestring(got_item, setted_text);

    itoa(ups_load, setted_text, 10);
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "ups");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "load");
    cJSON_SetValuestring(got_item, setted_text);

    itoa(actual_voltage, setted_text, 10);
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "output");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "voltage");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "_root");
    cJSON_SetValuestring(got_item, setted_text);

    if (audible_alarm_control == 2)
    {
        strcpy(setted_text, "enabled");
    }
    else
    {
        strcpy(setted_text, "disabled");
    }
    got_item = cJSON_GetObjectItemCaseSensitive(json_object, "ups");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "beeper");
    got_item = cJSON_GetObjectItemCaseSensitive(got_item, "status");
    cJSON_SetValuestring(got_item, setted_text);

    ESP_LOGI(TAG, "%s外部电源: %s, 充电: %s, 放电: %s, 蜂鸣器状态: %s, 电池电量: %d%%, 当前负载: %d%%, 剩余带机时长: %ds(%.2fmin)", alert_text, ac_present ? "ON" : "OFF", charging ? "Y" : "N", discharging ? "Y" : "N", audible_alarm_control == 2 ? "ON" : "OFF", battery_charge, ups_load, battery_runtime, 1.0 * battery_runtime / 60);
    
    if (all_good_flag && ac_present)
    {
        led_strip_set_pixel(led_strip, 0, 0, 0x03, 0);
        /* Refresh the strip to send data */
        led_strip_refresh(led_strip);
    }
    else
    {
        led_strip_set_pixel(led_strip, 0, 0x10, 0, 0);
        /* Refresh the strip to send data */
        led_strip_refresh(led_strip);
    }
    
    
}

/// @brief to recheck ups status and refresh json object. 
/// @param pvParameters 
void timer_task(void *pvParameters)
{
    timer_queue_element_t evt_queue;

    while (true)
    {
        if (xQueueReceive(timer_queue, &evt_queue, pdMS_TO_TICKS(50)))
        {
            // timer triggered.
            if (UPS_DEV_CONNECTED)
            {
                bool beep = false;
                refresh_ups_status_from_hid(&beep);
                if (!gpio_get_level(APP_QUIT_PIN))
                {
                    set_beep(!beep);
                }
            }
            else
            {
                led_strip_set_pixel(led_strip, 0, 0x08, 0x05, 0);
                /* Refresh the strip to send data */
                led_strip_refresh(led_strip);
                ESP_LOGI(TAG, "断开");
                cJSON *got_item;
                got_item = cJSON_GetObjectItemCaseSensitive(json_object, "ups");
                got_item = cJSON_GetObjectItemCaseSensitive(got_item, "status");
                cJSON_SetValuestring(got_item, "OFF");
            }
        }
    }
    xQueueReset(timer_queue);
    vQueueDelete(timer_queue);
    vTaskDelete(NULL);
}

/**
 * @brief HID Host main task
 *
 * Creates queue and get new event from the queue
 *
 * @param[in] pvParameters Not used
 */
void hid_host_task(void *pvParameters)
{
    hid_host_event_queue_t evt_queue;
    // Create queue
    hid_host_event_queue = xQueueCreate(10, sizeof(hid_host_event_queue_t));

    // Wait queue
    while (!user_shutdown)
    {
        if (xQueueReceive(hid_host_event_queue, &evt_queue, pdMS_TO_TICKS(50)))
        {
            hid_host_device_event(evt_queue.hid_device_handle,
                                  evt_queue.event,
                                  evt_queue.arg);
        }
    }

    xQueueReset(hid_host_event_queue);
    vQueueDelete(hid_host_event_queue);
    vTaskDelete(NULL);
}

/**
 * @brief HID Host Device callback
 *
 * Puts new HID Device event to the queue
 *
 * @param[in] hid_device_handle HID Device handle
 * @param[in] event             HID Device event
 * @param[in] arg               Not used
 */
void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                              const hid_host_driver_event_t event,
                              void *arg)
{
    const hid_host_event_queue_t evt_queue = {
        .hid_device_handle = hid_device_handle,
        .event = event,
        .arg = arg};
    xQueueSend(hid_host_event_queue, &evt_queue, 0);
}

// ==计时器 Timer

static bool IRAM_ATTR timer_on_alarm_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_ctx;
    timer_queue_element_t ele = {
        .event_count = edata->alarm_value};
    
    xQueueSendFromISR(queue, &ele, &high_task_awoken);
    return high_task_awoken == pdTRUE;
}
// ==计时器 Timer

static void configure_led(void)
{
    /* LED strip initialization with the GPIO and pixels number*/
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_PIN,
        .max_leds = 1, // at least one LED on board
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    /* Set all LED off to clear all pixels */
    led_strip_clear(led_strip);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    init_json_object();
    
    
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());


    SemaphoreHandle_t server_ready = xSemaphoreCreateBinary();
    assert(server_ready);
    xTaskCreate(tcp_server_task, "tcp_server", 4096, &server_ready, 5, NULL);
    xSemaphoreTake(server_ready, portMAX_DELAY);
    vSemaphoreDelete(server_ready);

    // == 定时器 Timer
    // Create queue
    timer_queue = xQueueCreate(10, sizeof(timer_queue_element_t));
    gptimer_handle_t gptimer = NULL;
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000, // 1MHz, 1 tick = 1us
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,                  // counter will reload with 0 on alarm event
        .alarm_count = 1000000,             // period = 1s @resolution 1MHz
        .flags.auto_reload_on_alarm = true, // enable auto-reload
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_on_alarm_callback,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, timer_queue));
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
    ESP_ERROR_CHECK(gptimer_start(gptimer));
    // == 定时器 Timer

    BaseType_t task_created;

    /*
     * Create usb_lib_task to:
     * - initialize USB Host library
     * - Handle USB Host events while APP pin in in HIGH state
     */
    task_created = xTaskCreatePinnedToCore(usb_lib_task,
                                           "usb_events",
                                           4096,
                                           xTaskGetCurrentTaskHandle(),
                                           2, NULL, 0);
    assert(task_created == pdTRUE);

    // Wait for notification from usb_lib_task to proceed
    ulTaskNotifyTake(false, 1000);

    /*
     * HID host driver configuration
     * - create background task for handling low level event inside the HID driver
     * - provide the device callback to get new HID Device connection event
     */
    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL};

    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    // Task is working until the devices are gone (while 'user_shutdown' if false)
    user_shutdown = false;

    /*
     * Create HID Host task process for handle events
     * IMPORTANT: Task is necessary here while there is no possibility to interact
     * with USB device from the callback.
     */
    task_created = xTaskCreate(&hid_host_task, "hid_task", 4 * 1024, NULL, 2, NULL);

    configure_led();

    // to receive queue sent from timer, and the actual thing is done at timer_task (for example, recheck ups status)
    task_created = xTaskCreate(&timer_task, "timer_task", 4 * 1024, NULL, 8, NULL);
    assert(task_created == pdTRUE);
}
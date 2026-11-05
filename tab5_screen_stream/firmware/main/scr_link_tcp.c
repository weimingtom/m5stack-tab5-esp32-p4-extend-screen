/*
 * TCP link over Wi-Fi: the Tab5 joins your AP and listens for one PC sender.
 *
 * Only one connection is served at a time - the sender is expected to reconnect
 * if the link drops.  Frames and status lines share the socket, exactly like the
 * USB transport, so the PC side is identical apart from the address.
 */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "scr_link.h"
#include "scr_wifi.h"

static const char *TAG = "link_tcp";

static volatile int s_sock = -1;

static void tcp_server_task(void *arg)
{
    while (true) {
        const int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd < 0) {
            ESP_LOGE(TAG, "socket() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        const int reuse = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(CONFIG_APP_TCP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 1) != 0) {
            ESP_LOGE(TAG, "bind/listen on port %d failed: errno %d", CONFIG_APP_TCP_PORT, errno);
            close(listen_fd);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "listening on tcp/%d", CONFIG_APP_TCP_PORT);

        while (true) {
            const int fd = accept(listen_fd, NULL, NULL);
            if (fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            const struct timeval recv_timeout = { .tv_sec = 1, .tv_usec = 0 };
            const struct timeval send_timeout = { .tv_sec = 2, .tv_usec = 0 };
            const int nodelay = 1;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

            ESP_LOGI(TAG, "sender connected");
            s_sock = fd;
            while (s_sock == fd) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            close(fd);
            s_sock = -1;
            ESP_LOGI(TAG, "sender disconnected, waiting for a new one");
        }
    }
}

esp_err_t scr_link_init(void)
{
    esp_err_t err;
    int attempt = 0;

    while ((err = scr_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "wifi bring-up failed (%s), retrying in 5 s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (++attempt >= 12) {
            return err;
        }
    }

    BaseType_t created = xTaskCreate(tcp_server_task, "tcp_server", 6144, NULL, 6, NULL);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

int scr_link_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    const int fd = s_sock;
    if (fd < 0) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms > 200 ? 200 : (timeout_ms ? timeout_ms : 1)));
        return 0;
    }

    const int n = recv(fd, buf, len, 0);
    if (n > 0) {
        return n;
    }
    if (n == 0) {
        s_sock = -1; /* peer closed */
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return 0;
    }
    s_sock = -1;
    return -1;
}

int scr_link_write(const void *buf, size_t len)
{
    const int fd = s_sock;
    if (fd < 0) {
        return -1;
    }

    size_t sent = 0;
    while (sent < len) {
        const int n = send(fd, (const uint8_t *)buf + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            s_sock = -1;
            return -1;
        }
        sent += (size_t)n;
    }
    return (int)sent;
}

bool scr_link_ready(void)
{
    return s_sock >= 0;
}

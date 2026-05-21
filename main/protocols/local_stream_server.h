#pragma once
#include <set>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_http_server.h>

class LocalStreamServer {
public:
    LocalStreamServer();
    ~LocalStreamServer();

    bool Start(int port = 80);
    void Stop();

    // Thread-safe: send binary frame to all connected WebSocket clients.
    // data must remain valid until function returns.
    void BroadcastBinary(const uint8_t* data, size_t len);

    size_t GetClientCount();

    // Called with true when /monitor?on=1, false when /monitor?on=0 or last client disconnects.
    void SetMonitorCallback(std::function<void(bool)> cb) { monitor_callback_ = std::move(cb); }

private:
    httpd_handle_t server_ = nullptr;
    SemaphoreHandle_t mutex_;
    std::set<int> client_fds_;
    bool monitoring_active_ = false;
    std::function<void(bool)> monitor_callback_;

    static LocalStreamServer* instance_;

    static esp_err_t HttpRootHandler(httpd_req_t* req);
    static esp_err_t WsAvHandler(httpd_req_t* req);
    static esp_err_t HttpMonitorHandler(httpd_req_t* req);

    void AddClient(int fd);
    void RemoveClient(int fd);

    struct SendWork {
        httpd_handle_t server;
        int fd;
        uint8_t* data;
        size_t len;
    };
    static void DoSend(void* arg);
};

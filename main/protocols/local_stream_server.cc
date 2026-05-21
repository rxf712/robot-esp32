#include "local_stream_server.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>

#define TAG "LocalStream"

// Minimal HTML player served at GET /
// JS: parse 8-byte StreamFrame header, render JPEG frames via canvas
static const char kPlayerHtml[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Camera Xiaozhi</title>"
"<style>"
"body{margin:0;background:#111;display:flex;flex-direction:column;"
"align-items:center;justify-content:center;min-height:100vh;color:#ccc;"
"font-family:monospace}"
"canvas{max-width:100vw;max-height:80vh;border:1px solid #333}"
"#st{margin:6px 0;font-size:13px}"
"#mb{margin:8px;padding:8px 20px;background:#555;color:#fff;"
"border:none;border-radius:4px;font-size:14px;cursor:pointer}"
"</style></head><body>"
"<div id='st'>Connecting...</div>"
"<canvas id='v'></canvas>"
"<button id='mb' onclick='toggleMon()'>监控模式：关闭</button>"
"<script>"
"const host=location.host;"
"const ws=new WebSocket('ws://'+host+'/av');"
"ws.binaryType='arraybuffer';"
"const canvas=document.getElementById('v');"
"const ctx=canvas.getContext('2d');"
"const st=document.getElementById('st');"
"const mb=document.getElementById('mb');"
"let frames=0,bytes=0,t0=Date.now(),mon=false;"
"function updateBtn(){mb.textContent='监控模式：'+(mon?'开启':'关闭');mb.style.background=mon?'#2a6e2a':'#555';}"
"function toggleMon(){mon=!mon;fetch('/monitor?on='+(mon?'1':'0'));updateBtn();}"
"ws.onopen=()=>{st.textContent='Connected';};"
"ws.onclose=()=>{st.textContent='Disconnected';if(mon){mon=false;updateBtn();}};"
"ws.onerror=()=>{st.textContent='Error';};"
"ws.onmessage=async(e)=>{"
"  const buf=e.data;"
"  if(buf.byteLength<8)return;"
"  const v=new DataView(buf);"
"  const type=v.getUint8(0);"
"  if(type!==1)return;"
"  const size=v.getUint16(2,false);"
"  if(buf.byteLength<8+size)return;"
"  const jpeg=buf.slice(8,8+size);"
"  try{"
"    const bm=await createImageBitmap(new Blob([jpeg],{type:'image/jpeg'}));"
"    if(canvas.width!==bm.width||canvas.height!==bm.height){"
"      canvas.width=bm.width;canvas.height=bm.height;"
"    }"
"    ctx.drawImage(bm,0,0);bm.close();"
"  }catch(err){}"
"  frames++;bytes+=size;"
"  const now=Date.now();"
"  if(now-t0>=1000){"
"    st.textContent=frames+' fps | '+(bytes/1024/frames).toFixed(1)+' KB/frame';"
"    frames=0;bytes=0;t0=now;"
"  }"
"};"
"window.addEventListener('beforeunload',()=>{if(mon)fetch('/monitor?on=0');});"
"</script></body></html>";

LocalStreamServer* LocalStreamServer::instance_ = nullptr;

LocalStreamServer::LocalStreamServer() {
    mutex_ = xSemaphoreCreateMutex();
    instance_ = this;
}

LocalStreamServer::~LocalStreamServer() {
    Stop();
    if (mutex_) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
    instance_ = nullptr;
}

bool LocalStreamServer::Start(int port) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 4;
    config.ctrl_port = 32780;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", port);
        return false;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HttpRootHandler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &root_uri);

    httpd_uri_t ws_uri = {
        .uri = "/av",
        .method = HTTP_GET,
        .handler = WsAvHandler,
        .user_ctx = nullptr,
        .is_websocket = true,
        .handle_ws_control_frames = true,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &ws_uri);

    httpd_uri_t mon_uri = {
        .uri = "/monitor",
        .method = HTTP_GET,
        .handler = HttpMonitorHandler,
        .user_ctx = nullptr,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(server_, &mon_uri);

    ESP_LOGI(TAG, "Streaming server started on port %d", port);
    return true;
}

void LocalStreamServer::Stop() {
    if (!server_) return;
    httpd_stop(server_);
    server_ = nullptr;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    client_fds_.clear();
    xSemaphoreGive(mutex_);
    ESP_LOGI(TAG, "Stopped");
}

esp_err_t LocalStreamServer::HttpRootHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, kPlayerHtml, strlen(kPlayerHtml));
    return ESP_OK;
}

esp_err_t LocalStreamServer::WsAvHandler(httpd_req_t* req) {
    if (!instance_) return ESP_FAIL;

    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS client connected: fd=%d", fd);
        instance_->AddClient(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) return ret;

    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS client closed: fd=%d", fd);
        instance_->RemoveClient(fd);
    }
    return ESP_OK;
}

void LocalStreamServer::AddClient(int fd) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    client_fds_.insert(fd);
    xSemaphoreGive(mutex_);
}

void LocalStreamServer::RemoveClient(int fd) {
    bool call_monitor_off = false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    client_fds_.erase(fd);
    if (client_fds_.empty() && monitoring_active_) {
        monitoring_active_ = false;
        call_monitor_off = true;
    }
    xSemaphoreGive(mutex_);
    if (call_monitor_off && monitor_callback_) {
        monitor_callback_(false);
    }
}

size_t LocalStreamServer::GetClientCount() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    size_t n = client_fds_.size();
    xSemaphoreGive(mutex_);
    return n;
}

void LocalStreamServer::DoSend(void* arg) {
    auto* work = static_cast<SendWork*>(arg);
    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_BINARY;
    pkt.payload = work->data;
    pkt.len = work->len;
    esp_err_t ret = httpd_ws_send_frame_async(work->server, work->fd, &pkt);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Send failed fd=%d err=%d (client disconnected)", work->fd, ret);
        if (instance_) instance_->RemoveClient(work->fd);
    }
    free(work->data);
    delete work;
}

esp_err_t LocalStreamServer::HttpMonitorHandler(httpd_req_t* req) {
    if (!instance_) return ESP_FAIL;

    char query[16];
    char val[4];
    bool active = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "on", val, sizeof(val)) == ESP_OK) {
        active = (val[0] == '1');
    }

    bool changed = false;
    xSemaphoreTake(instance_->mutex_, portMAX_DELAY);
    if (instance_->monitoring_active_ != active) {
        instance_->monitoring_active_ = active;
        changed = true;
    }
    xSemaphoreGive(instance_->mutex_);

    if (changed && instance_->monitor_callback_) {
        instance_->monitor_callback_(active);
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, active ? "on" : "off");
    return ESP_OK;
}

void LocalStreamServer::BroadcastBinary(const uint8_t* data, size_t len) {
    if (!server_) return;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    std::set<int> fds = client_fds_;
    xSemaphoreGive(mutex_);

    if (fds.empty()) return;

    for (int fd : fds) {
        auto* work = new SendWork;
        work->server = server_;
        work->fd = fd;
        work->data = (uint8_t*)malloc(len);
        if (!work->data) { delete work; continue; }
        memcpy(work->data, data, len);
        work->len = len;
        esp_err_t ret = httpd_queue_work(server_, DoSend, work);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "httpd_queue_work failed: %d", ret);
            free(work->data);
            delete work;
        }
    }
}

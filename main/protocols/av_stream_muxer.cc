#include "av_stream_muxer.h"
#include "websocket_protocol.h"

#include <arpa/inet.h>
#include <esp_log.h>
#include <cstring>
#include <vector>

#define TAG "AvMuxer"

void AvStreamMuxer::SetMode(AvStreamMode mode) {
    mode_ = mode;
    ESP_LOGI(TAG, "Mode set to %d", (int)mode);
}

void AvStreamMuxer::SetLocalServer(LocalStreamServer* server) {
    local_server_ = server;
}

void AvStreamMuxer::SetRemoteProtocol(WebsocketProtocol* protocol) {
    remote_protocol_ = protocol;
}

void AvStreamMuxer::SendVideo(std::unique_ptr<VideoStreamPacket> pkt) {
    if (mode_ == kAvStreamOff || !pkt) return;

    if (mode_ == kAvStreamLocal && local_server_) {
        SendToLocal(*pkt);
    } else if (mode_ == kAvStreamRemote && remote_protocol_) {
        SendToRemote(std::move(pkt));
    }
}

void AvStreamMuxer::SendToLocal(const VideoStreamPacket& pkt) {
    if (!local_server_) return;
    size_t total = sizeof(StreamFrame) + pkt.jpeg_payload.size();
    std::vector<uint8_t> buf(total);
    auto* hdr = reinterpret_cast<StreamFrame*>(buf.data());
    hdr->type = 1;  // video
    hdr->flags = pkt.keyframe ? 0x01 : 0x00;
    hdr->payload_size = htons((uint16_t)pkt.jpeg_payload.size());
    hdr->timestamp_ms = htonl(pkt.timestamp_ms);
    memcpy(hdr->payload, pkt.jpeg_payload.data(), pkt.jpeg_payload.size());
    local_server_->BroadcastBinary(buf.data(), buf.size());
}

void AvStreamMuxer::SendToRemote(std::unique_ptr<VideoStreamPacket> pkt) {
    if (!remote_protocol_) return;
    remote_protocol_->SendVideo(std::move(pkt));
}

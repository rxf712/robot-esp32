#pragma once
#include <memory>
#include "protocol.h"
#include "local_stream_server.h"

class WebsocketProtocol;

class AvStreamMuxer {
public:
    void SetMode(AvStreamMode mode);
    void SetLocalServer(LocalStreamServer* server);
    void SetRemoteProtocol(WebsocketProtocol* protocol);

    void SendVideo(std::unique_ptr<VideoStreamPacket> pkt);

private:
    AvStreamMode mode_ = kAvStreamOff;
    LocalStreamServer* local_server_ = nullptr;
    WebsocketProtocol* remote_protocol_ = nullptr;

    void SendToLocal(const VideoStreamPacket& pkt);
    void SendToRemote(std::unique_ptr<VideoStreamPacket> pkt);
};

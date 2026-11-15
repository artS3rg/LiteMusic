#pragma once

#include "music.hpp"
#include <chrono>
#include <memory>

struct RoomPlayback {
    Track track;
    double seconds = 0;
    bool playing = false;
    std::chrono::steady_clock::time_point sampled_at = std::chrono::steady_clock::now();

    double position() const;
};

struct RoomView {
    enum Mode { idle, connecting, host, guest } mode = idle;
    std::string code;
    std::string error;
    std::optional<RoomPlayback> playback;
    unsigned revision = 0;
};

class RoomClient {
public:
    RoomClient();
    ~RoomClient();

    void create();
    void join(std::string code);
    void leave();
    RoomView view() const;
    void publish(std::optional<RoomPlayback> playback);

private:
    struct Session;
    std::unique_ptr<Session> session_;
};

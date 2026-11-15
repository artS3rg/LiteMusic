#pragma once

#include "music.hpp"
#include <memory>

class DiscordPresence {
public:
    DiscordPresence();
    ~DiscordPresence();

    bool enabled() const { return enabled_; }
    void set_enabled(bool enabled);
    void playing(const Track& track, double position, double duration, bool seek = false);
    void clear();

private:
    struct Session;
    bool enabled_ = true;
    std::unique_ptr<Session> session_;
};

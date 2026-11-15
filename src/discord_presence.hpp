#pragma once

#include "music.hpp"
#include <memory>

bool valid_discord_id(std::string_view id);

class DiscordPresence {
public:
    DiscordPresence();
    ~DiscordPresence();

    // Setting an empty ID disables sharing and clears any activity.
    void configure(std::string application_id);
    void playing(const Track& track, double position, double duration, bool seek = false);
    void clear();

private:
    struct Session;
    std::string application_id_;
    std::unique_ptr<Session> session_;
};

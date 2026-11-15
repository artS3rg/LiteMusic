#pragma once

#include "music.hpp"
#include <cstdint>
#include <optional>

struct ResumeTrack {
    std::string account;
    Track track;
    std::int64_t position = 0;
};

int load_volume();
void save_volume(int volume);
std::optional<ResumeTrack> load_resume_track();
void save_resume_track(const ResumeTrack& resume);
void erase_resume_track();
std::string load_discord_id();
void save_discord_id(const std::string& id);

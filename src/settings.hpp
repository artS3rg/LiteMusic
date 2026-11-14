#pragma once

#include "music.hpp"
#include <optional>

struct ResumeTrack {
    std::string account;
    Track track;
    LONGLONG position = 0;
};

int load_volume();
void save_volume(int volume);
std::optional<ResumeTrack> load_resume_track();
void save_resume_track(const ResumeTrack& resume);
void erase_resume_track();

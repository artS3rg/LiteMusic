#pragma once

#include <memory>
#include <string>

struct Playback {
    enum State { empty, loading, playing, paused, ended, failed } state = empty;
    double position = 0;
    double duration = 0;
};

class Player {
public:
    explicit Player(bool silent_test = false);
    ~Player();
    void open(const std::string& url, double position = 0);
    void clear();
    void play(bool enabled);
    void seek(double seconds);
    void volume(double level);
    Playback poll();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

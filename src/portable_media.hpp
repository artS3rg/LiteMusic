#pragma once

#include <functional>
#include <memory>
#include <string>

enum class MediaAction { play, pause, toggle, next, previous, stop, raise, volume, seek };

struct MediaState {
    enum class Status { stopped, paused, playing };
    Status status = Status::stopped;
    bool can_play = false;
    bool can_pause = false;
    bool can_next = false;
    bool can_previous = false;
    bool can_seek = false;
    std::string track_id, title, artist;
    double duration = 0;
    double position = 0;
    double volume = 1;
    bool operator==(const MediaState&) const = default;
};

class MediaKeys {
public:
    using Handler = std::function<void(MediaAction, double)>;
    explicit MediaKeys(Handler handler);
    ~MediaKeys();
    void update(MediaState state);
    void seeked(double seconds);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

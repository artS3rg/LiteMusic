#pragma once

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct Track {
    std::string id;
    std::wstring name;
    std::wstring artists;
};

struct Playlist {
    std::string owner, kind;
    std::wstring name;
};

struct Profile {
    std::string uid;
    std::wstring name;
};

struct DeviceCode {
    std::wstring url, code;
};

struct Audio {
    std::wstring url, format;
};

struct Wave {
    std::string batch;
    std::vector<Track> tracks;
};

struct TracksPage {
    std::vector<Track> tracks;
    bool more = false;
};

class HttpError : public std::runtime_error {
public:
    unsigned long status;
    HttpError(unsigned long status, std::string message);
};

std::wstring widen(std::string_view input);
std::string narrow(std::wstring_view input);
std::string saved_token();
void save_token(const std::string& token);
void forget_token();

class Music {
public:
    explicit Music(std::string token = {});
    static std::string sign_in(const std::function<bool()>& canceled, const std::function<void(DeviceCode)>& notify);
    Profile profile() const;
    TracksPage search(const std::wstring& query, size_t page) const;
    TracksPage favorites(const std::string& uid, size_t page) const;
    std::vector<std::string> liked_tracks(const std::string& uid) const;
    void set_track_like(const std::string& uid, const std::string& track, bool liked) const;
    std::vector<Playlist> playlists(const std::string& uid) const;
    TracksPage playlist(const Playlist& list, size_t page) const;
    Audio stream(const std::string& track) const;
    Wave wave(const std::string& previous = {}) const;
    void wave_event(std::string_view type, const std::string& batch = {},
                    const std::string& track = {}, double played_seconds = 0) const;

private:
    std::string token_;
};

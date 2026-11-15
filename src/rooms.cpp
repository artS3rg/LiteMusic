#include "rooms.hpp"
#include "music_platform.hpp"
#include "cJSON.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr double max_position = 86400;

#ifndef LITEMUSIC_ROOMS_URL
#define LITEMUSIC_ROOMS_URL ""
#endif

struct Endpoint {
    std::string host;
    unsigned short port = 443;
    bool secure = true;
};

std::optional<Endpoint> rooms_endpoint() {
    auto* override_url = std::getenv("LITEMUSIC_ROOMS_URL");
    std::string_view url = override_url && *override_url ? override_url : LITEMUSIC_ROOMS_URL;
    Endpoint address;
    if (url.starts_with("https://")) url.remove_prefix(8);
    else if (url.starts_with("http://127.0.0.1:")) {
        url.remove_prefix(7);
        address.secure = false;
    } else return {};
    if (url.ends_with('/')) url.remove_suffix(1);
    auto colon = url.find(':');
    auto host = url.substr(0, colon);
    if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string_view::npos) {
        return {};
    }
    address.host = host;
    if (colon != std::string_view::npos) {
        auto port = url.substr(colon + 1);
        auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), address.port);
        if (error != std::errc{} || end != port.data() + port.size() || address.port == 0) return {};
    }
    return address;
}

std::string text(const cJSON* node, const char* key) {
    const char* value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(node, key));
    return value ? value : "";
}

std::string room_code(std::string code) {
    std::erase_if(code, [](char ch) { return ch == '-' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; });
    for (char& ch : code) if (ch >= 'a' && ch <= 'f') ch -= 'a' - 'A';
    if (code.size() != 12 || code.find_first_not_of("0123456789ABCDEF") != std::string::npos) return {};
    return code;
}

std::string label(std::wstring_view value) {
    auto result = narrow(value);
    if (result.size() > 180) {
        size_t end = 180;
        while ((static_cast<unsigned char>(result[end]) & 0xc0) == 0x80) --end;
        result.resize(end);
    }
    return result;
}

Json checked_reply(const Endpoint& target, const std::string& path, std::string_view body) {
    HttpResponse response;
    try {
        response = https_request(target.host.c_str(), path, {}, body, true, target.port, target.secure, 3000);
    } catch (const std::runtime_error& e) {
        throw HttpError(0, e.what());
    }
    if (response.status == 404) throw HttpError(404, "Комната закрыта или код не найден.");
    if (response.status < 200 || response.status >= 300) {
        throw HttpError(response.status, "Сервер комнат вернул HTTP " + std::to_string(response.status));
    }
    Json reply(cJSON_ParseWithLengthOpts(response.body.c_str(), response.body.size() + 1, nullptr, true), cJSON_Delete);
    if (!cJSON_IsObject(reply.get())) throw std::runtime_error("Некорректный ответ сервера комнат.");
    return reply;
}

std::string body_for(std::string_view key, const std::optional<RoomPlayback>& playback, bool include_state) {
    Json body(cJSON_CreateObject(), cJSON_Delete);
    if (!body || !cJSON_AddStringToObject(body.get(), "host_key", std::string(key).c_str())) throw std::bad_alloc();
    if (include_state) {
        if (!playback) {
            if (!cJSON_AddNullToObject(body.get(), "state")) throw std::bad_alloc();
        } else {
            auto* state = cJSON_AddObjectToObject(body.get(), "state");
            if (!state || !cJSON_AddStringToObject(state, "id", playback->track.id.c_str()) ||
                !cJSON_AddStringToObject(state, "title", label(playback->track.name).c_str()) ||
                !cJSON_AddStringToObject(state, "artist", label(playback->track.artists).c_str()) ||
                !cJSON_AddNumberToObject(state, "position_ms", std::llround(playback->position() * 1000)) ||
                !cJSON_AddBoolToObject(state, "playing", playback->playing)) throw std::bad_alloc();
        }
    }
    std::unique_ptr<char, decltype(&cJSON_free)> encoded(cJSON_PrintUnformatted(body.get()), cJSON_free);
    if (!encoded) throw std::bad_alloc();
    return encoded.get();
}
}

double RoomPlayback::position() const {
    auto elapsed = playing ? std::chrono::duration<double>(Clock::now() - sampled_at).count() : 0;
    return std::clamp(seconds + elapsed, 0.0, max_position);
}

struct RoomClient::Session {
    const std::optional<Endpoint> target = rooms_endpoint();
    mutable std::mutex mutex;
    std::condition_variable changed;
    RoomView current;
    std::optional<RoomPlayback> outgoing;
    unsigned revision = 0;
    bool stopping = false;
    std::thread worker;

    ~Session() {
        leave();
        if (worker.joinable()) worker.join();
    }

    void leave() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_one();
    }

    void start(bool owner, std::string code) {
        {
            std::lock_guard lock(mutex);
            if (current.mode != RoomView::idle) return;
        }
        if (worker.joinable()) worker.join();
        std::lock_guard lock(mutex);
        if (!target) {
            current.error = "Сервер комнат не настроен.";
            return;
        }
        current = {RoomView::connecting};
        outgoing.reset();
        stopping = false;
        revision = 1;
        worker = std::thread([this, owner, code = std::move(code)] {
            std::string path, key, error;
            try {
                if (owner) {
                    auto reply = checked_reply(*target, "/v1/rooms", "{}");
                    auto assigned = room_code(text(reply.get(), "code"));
                    key = text(reply.get(), "host_key");
                    if (assigned.empty() || key.size() != 48 || key.find_first_not_of("0123456789abcdef") != std::string::npos) {
                        throw std::runtime_error("Сервер не выдал код комнаты.");
                    }
                    path = "/v1/rooms/" + assigned;
                    {
                        std::lock_guard guard(mutex);
                        current = {RoomView::host, std::move(assigned)};
                    }
                    host_loop(path, key);
                } else {
                    path = "/v1/rooms/" + code;
                    guest_loop(code, path);
                }
            } catch (const std::exception& e) {
                error = e.what();
            }
            if (owner && !path.empty() && !key.empty()) {
                try { checked_reply(*target, path + "/close", body_for(key, {}, false)); } catch (const std::exception&) {}
            }
            std::lock_guard guard(mutex);
            current = {RoomView::idle, {}, stopping ? "" : std::move(error)};
        });
    }

    void host_loop(const std::string& path, const std::string& key) {
        unsigned sent = 0;
        for (;;) {
            std::optional<RoomPlayback> playback;
            unsigned version;
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, 12s, [&] { return stopping || revision != sent; });
                if (stopping) return;
                playback = outgoing;
                version = revision;
            }
            auto update = [&, this] {
                return checked_reply(*target, path + "/update", body_for(key, playback, version != sent));
            };
            if (!retry_request(update)) return;
            sent = version;
        }
    }

    template<class Request>
    Json retry_request(const Request& request) {
        for (unsigned attempt = 0; ; ++attempt) {
            try {
                return request();
            } catch (const HttpError& e) {
                if (attempt >= 2 || (e.status != 0 && e.status != 429 && e.status < 500)) throw;
            }
            std::unique_lock lock(mutex);
            if (changed.wait_for(lock, 1s, [&] { return stopping; })) return {nullptr, cJSON_Delete};
        }
    }

    void guest_loop(const std::string& code, const std::string& path) {
        for (;;) {
            Clock::time_point began;
            auto reply = retry_request([&] {
                began = Clock::now();
                return checked_reply(*target, path + "/read", "{}");
            });
            if (!reply) return;
            auto received = Clock::now();
            std::optional<RoomPlayback> playback;
            auto* state = cJSON_GetObjectItemCaseSensitive(reply.get(), "state");
            auto* version = cJSON_GetObjectItemCaseSensitive(reply.get(), "revision");
            if (!cJSON_IsNumber(version) || version->valuedouble < 0 || version->valuedouble > 1e9) {
                throw std::runtime_error("Некорректная версия состояния комнаты.");
            }
            if (!cJSON_IsNull(state)) {
                Track track{text(state, "id"), widen(text(state, "title")), widen(text(state, "artist")), Catalog::audius};
                auto* position = cJSON_GetObjectItemCaseSensitive(state, "position_ms");
                auto* playing = cJSON_GetObjectItemCaseSensitive(state, "playing");
                if (track.id.empty() || track.name.empty() || !cJSON_IsNumber(position) || !cJSON_IsBool(playing) ||
                    position->valuedouble < 0 || position->valuedouble > max_position * 1000) {
                    throw std::runtime_error("Некорректное состояние воспроизведения комнаты.");
                }
                bool is_playing = cJSON_IsTrue(playing);
                double delay = is_playing ? std::chrono::duration<double>(received - began).count() / 2 : 0;
                playback = RoomPlayback{std::move(track), position->valuedouble / 1000 + delay, is_playing, received};
            }
            std::unique_lock lock(mutex);
            if (stopping) return;
            current = {RoomView::guest, code, {}, std::move(playback), static_cast<unsigned>(version->valuedouble)};
            if (changed.wait_for(lock, 650ms, [&] { return stopping; })) return;
        }
    }
};

RoomClient::RoomClient() : session_(std::make_unique<Session>()) {}
RoomClient::~RoomClient() = default;

bool RoomClient::available() const { return session_->target.has_value(); }
void RoomClient::create() { session_->start(true, {}); }
void RoomClient::join(std::string code) {
    code = room_code(std::move(code));
    if (!code.empty()) session_->start(false, std::move(code));
    else {
        std::lock_guard lock(session_->mutex);
        session_->current.error = "Проверьте код комнаты.";
    }
}
void RoomClient::leave() { session_->leave(); }

RoomView RoomClient::view() const {
    std::lock_guard lock(session_->mutex);
    return session_->current;
}

void RoomClient::publish(std::optional<RoomPlayback> playback) {
    {
        std::lock_guard lock(session_->mutex);
        if (session_->current.mode != RoomView::host || session_->stopping) return;
        const auto& old = session_->outgoing;
        if (!old && !playback) return;
        if (old && playback && old->track.id == playback->track.id && old->playing == playback->playing &&
            std::abs(old->position() - playback->position()) < 0.35) return;
        session_->outgoing = std::move(playback);
        ++session_->revision;
    }
    session_->changed.notify_one();
}

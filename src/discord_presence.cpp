#include "discord_presence.hpp"
#include "cJSON.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono;
using Clock = steady_clock;
constexpr auto io_timeout = 600ms;
constexpr size_t max_payload = 64 * 1024;
enum Opcode : std::uint32_t { handshake, frame, close_connection, ping, pong };

class Pipe {
public:
    ~Pipe() { close(); }
    bool open();
    void close();
    int available() const;  // -1 on disconnect, 0 when idle, 1 when input is ready.

    bool send(Opcode op, std::string_view payload, Clock::time_point deadline) {
        std::string bytes;
        for (auto value : {std::uint32_t(op), std::uint32_t(payload.size())}) {
            for (int shift = 0; shift < 32; shift += 8) bytes += static_cast<char>(value >> shift);
        }
        bytes += payload;
        return transfer(bytes.data(), bytes.size(), true, deadline);
    }

    bool receive(Opcode& op, std::string& payload, Clock::time_point deadline) {
        unsigned char header[8];
        if (!transfer(reinterpret_cast<char*>(header), sizeof(header), false, deadline)) return false;
        std::uint32_t fields[2]{};
        for (size_t i = 0; i < 8; ++i) fields[i / 4] |= std::uint32_t(header[i]) << ((i % 4) * 8);
        if (fields[1] > max_payload) return false;
        op = static_cast<Opcode>(fields[0]);
        payload.resize(fields[1]);
        return transfer(payload.data(), payload.size(), false, deadline);
    }

private:
#ifdef _WIN32
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
#else
    int fd_ = -1;
#endif
    bool transfer(char* bytes, size_t count, bool write, Clock::time_point deadline);
};

int time_left(Clock::time_point deadline) {
    return static_cast<int>(std::max(0ms, ceil<milliseconds>(deadline - Clock::now())).count());
}

#ifdef _WIN32
bool Pipe::open() {
    for (int i = 0; i < 10; ++i) {
        auto path = LR"(\\.\pipe\discord-ipc-)" + std::to_wstring(i);
        pipe_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe_ == INVALID_HANDLE_VALUE) continue;
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event_) return true;
        close();
        break;
    }
    return false;
}

void Pipe::close() {
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(pipe_, INVALID_HANDLE_VALUE));
    if (event_) CloseHandle(std::exchange(event_, nullptr));
}

int Pipe::available() const {
    DWORD size = 0;
    if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &size, nullptr)) return -1;
    return size > 0 ? 1 : 0;
}

bool Pipe::transfer(char* bytes, size_t count, bool write, Clock::time_point deadline) {
    while (count) {
        int timeout = time_left(deadline);
        if (!timeout) return false;
        ResetEvent(event_);
        OVERLAPPED operation{};
        operation.hEvent = event_;
        DWORD done = 0;
        auto size = static_cast<DWORD>(count);
        BOOL completed = write ? WriteFile(pipe_, bytes, size, &done, &operation) :
                                 ReadFile(pipe_, bytes, size, &done, &operation);
        if (!completed) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(event_, static_cast<DWORD>(timeout)) != WAIT_OBJECT_0) {
                CancelIoEx(pipe_, &operation);
                GetOverlappedResult(pipe_, &operation, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe_, &operation, &done, FALSE)) return false;
        }
        if (!done) return false;
        bytes += done;
        count -= done;
    }
    return true;
}
#else
bool Pipe::open() {
    std::vector<std::string> directories;
    for (auto key : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        if (auto* dir = std::getenv(key); dir && dir[0] == '/' &&
            std::find(directories.begin(), directories.end(), dir) == directories.end()) directories.emplace_back(dir);
    }
    directories.emplace_back("/tmp");
    for (const auto& dir : directories) {
        for (const auto& prefix : {dir, dir + "/app/com.discordapp.Discord"}) {
            for (int i = 0; i < 10; ++i) {
                auto path = prefix + "/discord-ipc-" + std::to_string(i);
                sockaddr_un addr{};
                if (path.size() >= sizeof(addr.sun_path)) continue;
                addr.sun_family = AF_UNIX;
                std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
                auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
#ifdef __APPLE__
                addr.sun_len = static_cast<unsigned char>(length);
#endif
                fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd_ < 0) return false;
                if (fcntl(fd_, F_SETFD, FD_CLOEXEC) == 0 && fcntl(fd_, F_SETFL, O_NONBLOCK) == 0 &&
                    connect(fd_, reinterpret_cast<sockaddr*>(&addr), length) == 0) {
#ifdef __APPLE__
                    int yes = 1;
                    if (setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0) {
                        close();
                        return false;
                    }
#endif
                    return true;
                }
                close();
            }
        }
    }
    return false;
}

void Pipe::close() {
    if (fd_ >= 0) ::close(std::exchange(fd_, -1));
}

int Pipe::available() const {
    pollfd input{fd_, POLLIN, 0};
    int result = poll(&input, 1, 0);
    if (!result || (result < 0 && errno == EINTR)) return 0;
    if (result < 0 || (input.revents & (POLLERR | POLLNVAL))) return -1;
    if (input.revents & POLLIN) return 1;
    return -1;
}

bool Pipe::transfer(char* bytes, size_t count, bool write, Clock::time_point deadline) {
    while (count) {
        int timeout = time_left(deadline);
        if (!timeout) return false;
        pollfd socket_ready{fd_, static_cast<short>(write ? POLLOUT : POLLIN), 0};
        int result = poll(&socket_ready, 1, timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || !(socket_ready.revents & socket_ready.events)) return false;
#ifdef __APPLE__
        constexpr int flags = 0;
#else
        constexpr int flags = MSG_NOSIGNAL;
#endif
        auto transferred = write ? ::send(fd_, bytes, count, flags) : recv(fd_, bytes, count, 0);
        if (transferred < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (transferred <= 0) return false;
        bytes += transferred;
        count -= static_cast<size_t>(transferred);
    }
    return true;
}
#endif

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::string_view field(const cJSON* object, const char* key) {
    auto* string = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
    return string ? string : "";
}

bool wait_reply(Pipe& pipe, std::string_view nonce) {
    const auto deadline = Clock::now() + io_timeout;
    std::string payload;
    Opcode op;
    while (pipe.receive(op, payload, deadline)) {
        if (op == close_connection) return false;
        if (op == ping && !pipe.send(pong, payload, deadline)) return false;
        if (op != frame) continue;
        Json data(cJSON_ParseWithLength(payload.c_str(), payload.size() + 1), cJSON_Delete);
        if (nonce.empty() && field(data.get(), "evt") == "READY") return true;
        if (!nonce.empty() && field(data.get(), "nonce") == nonce) return field(data.get(), "evt") != "ERROR";
    }
    return false;
}

struct Activity {
    std::string id, title, artist;
    std::int64_t start = 0, end = 0;
};

// Keep UTF-8 whole when Discord's field limit requires shortening a title.
std::string short_text(std::wstring_view value) {
    auto result = narrow(value);
    if (result.size() > 128) {
        size_t size = 128;
        while ((static_cast<unsigned char>(result[size]) & 0xC0) == 0x80) --size;
        result.resize(size);
    }
    return result;
}

std::string command(const std::optional<Activity>& track, const std::string& nonce) {
#ifdef _WIN32
    auto pid = GetCurrentProcessId();
#else
    auto pid = getpid();
#endif
    Json root(cJSON_CreateObject(), cJSON_Delete);
    if (!root || !cJSON_AddStringToObject(root.get(), "cmd", "SET_ACTIVITY") ||
        !cJSON_AddStringToObject(root.get(), "nonce", nonce.c_str())) return {};
    auto* args = cJSON_AddObjectToObject(root.get(), "args");
    if (!args || !cJSON_AddNumberToObject(args, "pid", pid)) return {};
    if (!track) {
        if (!cJSON_AddNullToObject(args, "activity")) return {};
    } else {
        auto* activity = cJSON_AddObjectToObject(args, "activity");
        if (!activity || !cJSON_AddNumberToObject(activity, "type", 2) ||
            !cJSON_AddNumberToObject(activity, "status_display_type", 2) ||
            !cJSON_AddStringToObject(activity, "details", track->title.c_str()) ||
            (!track->artist.empty() && !cJSON_AddStringToObject(activity, "state", track->artist.c_str()))) return {};
        auto* stamps = cJSON_AddObjectToObject(activity, "timestamps");
        if (!stamps || !cJSON_AddNumberToObject(stamps, "start", static_cast<double>(track->start)) ||
            (track->end > 0 && !cJSON_AddNumberToObject(stamps, "end", static_cast<double>(track->end)))) return {};
    }
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(root.get()), cJSON_free);
    return json ? json.get() : "";
}
}

struct DiscordPresence::Session {
    std::string id;
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Activity> activity;
    size_t revision = 0;
    bool closing = false;
    std::thread worker;

    explicit Session(std::string id) : id(std::move(id)), worker([this] {
        try { run(); } catch (const std::exception&) {}
    }) {}

    ~Session() {
        {
            std::lock_guard lock(mutex);
            closing = true;
        }
        changed.notify_one();
        worker.join();
    }

    void run() {
        Pipe pipe;
        bool connected = false;
        size_t sent = 0;
        std::uint64_t sequence = 0;
        auto reconnect_at = Clock::now();
        auto next_update = Clock::now();
        for (;;) {
            std::optional<Activity> snapshot;
            size_t version;
            {
                std::unique_lock lock(mutex);
                if (!connected && !activity && revision == sent) {
                    changed.wait(lock, [&] { return closing || revision != sent; });
                } else if (!closing) {
                    changed.wait_for(lock, connected ? 1s : 3s, [&] {
                        return closing || (revision != sent &&
                            (!activity || (connected ? Clock::now() >= next_update : Clock::now() >= reconnect_at)));
                    });
                }
                if (closing) break;
                snapshot = activity;
                version = revision;
            }
            if (!snapshot) {
                if (connected) {
                    auto payload = command({}, std::to_string(++sequence));
                    if (!payload.empty()) pipe.send(frame, payload, Clock::now() + io_timeout);
                    pipe.close();
                    connected = false;
                }
                sent = version;
                reconnect_at = Clock::now();
                continue;
            }
            if (!connected && Clock::now() >= reconnect_at) {
                auto hello = R"({"v":1,"client_id":")" + id + "\"}";
                connected = pipe.open() && pipe.send(handshake, hello, Clock::now() + io_timeout) && wait_reply(pipe, {});
                if (!connected) pipe.close();
                else sent = 0;
            }
            if (connected && version != sent && Clock::now() >= next_update) {
                // Connect and handshake run without holding the UI lock.
                {
                    std::lock_guard lock(mutex);
                    if (closing || revision != version) continue;
                }
                auto nonce = std::to_string(++sequence);
                auto payload = command(snapshot, nonce);
                connected = !payload.empty() && pipe.send(frame, payload, Clock::now() + io_timeout) && wait_reply(pipe, nonce);
                if (connected) {
                    sent = version;
                    next_update = Clock::now() + 4s;
                }
            }
            if (connected) {
                int pending = pipe.available();
                if (pending < 0) connected = false;
                if (pending > 0) {
                    Opcode op;
                    std::string payload;
                    auto deadline = Clock::now() + io_timeout;
                    connected = pipe.receive(op, payload, deadline) && op != close_connection &&
                                (op != ping || pipe.send(pong, payload, deadline));
                }
            }
            if (!connected) {
                pipe.close();
                if (Clock::now() >= reconnect_at) reconnect_at = Clock::now() + 5s;
            }
        }
        if (connected) {
            auto payload = command({}, std::to_string(++sequence));
            if (!payload.empty()) pipe.send(frame, payload, Clock::now() + io_timeout);
        }
    }
};

bool valid_discord_id(std::string_view id) {
    return id.size() >= 17 && id.size() <= 20 && id.front() != '0' &&
           id.find_first_not_of("0123456789") == std::string_view::npos &&
           (id.size() < 20 || id <= "18446744073709551615");
}

DiscordPresence::DiscordPresence() = default;
DiscordPresence::~DiscordPresence() = default;

void DiscordPresence::configure(std::string application_id) {
    if (application_id_ == application_id) return;
    session_.reset();
    application_id_ = valid_discord_id(application_id) ? std::move(application_id) : std::string{};
}

void DiscordPresence::playing(const Track& track, double position, double duration, bool seek) {
    if (application_id_.empty() || track.id.empty() || track.name.empty()) return;
    if (!session_) session_ = std::make_unique<Session>(application_id_);
    position = std::isfinite(position) ? std::clamp(position, 0.0, 86400.0) : 0;
    duration = std::isfinite(duration) ? std::clamp(duration, 0.0, 86400.0) : 0;
    auto start = duration_cast<seconds>(system_clock::now().time_since_epoch()).count() - std::llround(position);
    auto end = duration > 0 ? start + std::max(1LL, std::llround(duration)) : 0;
    {
        std::lock_guard lock(session_->mutex);
        auto& old = session_->activity;
        if (!seek && old && old->id == track.id && std::abs(old->start - start) <= 2 &&
            (old->end != 0) == (end != 0)) return;
        old = Activity{track.id, short_text(track.name), short_text(track.artists), start, end};
        ++session_->revision;
    }
    session_->changed.notify_one();
}

void DiscordPresence::clear() {
    if (!session_) return;
    {
        std::lock_guard lock(session_->mutex);
        if (!session_->activity) return;
        session_->activity.reset();
        ++session_->revision;
    }
    session_->changed.notify_one();
}

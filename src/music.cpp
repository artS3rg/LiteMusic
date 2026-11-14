#include "music.hpp"

#include <bcrypt.h>
#include <wincred.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <objbase.h>

#include "cJSON.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>

using namespace std::chrono_literals;

namespace {
constexpr wchar_t api_host[] = L"api.music.yandex.net";
constexpr wchar_t oauth_host[] = L"oauth.yandex.ru";
constexpr wchar_t credential_name[] = L"LiteMusic/YandexMusic";
constexpr std::string_view station = "/rotor/station/user:onyourwave";
constexpr size_t page_size = 50;

// Public desktop wire constants documented by MarshalX/yandex-music-api.
constexpr char signing_key[] = "kzqU4XhfCaY6B6JTHODeq5";
constexpr wchar_t music_client[] = L"YandexMusicDesktopAppWindows/5.95.0";
constexpr char oauth_id[] = "23cabbbdc6cd418abb4b39c32c41195d";
constexpr char oauth_key[] = "53bc75238f0c4d08a118e51fe9203300";

using Internet = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

[[noreturn]] void system_error(const char* action, DWORD code = GetLastError()) {
    throw std::runtime_error(std::string(action) + " (Windows " + std::to_string(code) + ")");
}

const cJSON* field(const cJSON* node, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(node, name);
}

std::string value(const cJSON* node) {
    if (cJSON_IsString(node)) return cJSON_GetStringValue(node);
    // API identifiers and paging values are decimal integers.
    if (cJSON_IsNumber(node) && node->valuedouble >= 0 && node->valuedouble <= 9007199254740991.0) {
        return std::to_string(static_cast<unsigned long long>(node->valuedouble));
    }
    return {};
}

bool digits(std::string_view str) {
    return !str.empty() && std::all_of(str.begin(), str.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string encode(std::string_view str) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : str) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

Json request(const wchar_t* host, std::string_view path, const std::string& token = {},
             const std::string& body = {}, bool json_body = false) {
    Internet session(WinHttpOpen(L"LiteMusic/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
    if (!session) system_error("WinHTTP");
    WinHttpSetTimeouts(session.get(), 4000, 5000, 6000, 8000);
    Internet connection(WinHttpConnect(session.get(), host, INTERNET_DEFAULT_HTTPS_PORT, 0), WinHttpCloseHandle);
    if (!connection) system_error("WinHttpConnect");
    auto target = widen(path);
    Internet call(WinHttpOpenRequest(connection.get(), body.empty() ? L"GET" : L"POST", target.c_str(),
                                    nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE),
                  WinHttpCloseHandle);
    if (!call) system_error("WinHttpOpenRequest");

    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(call.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects))) {
        system_error("WinHttpSetOption");
    }
    std::wstring headers = L"Accept: application/json\r\nAccept-Language: ru\r\n";
    if (!token.empty()) {
        headers += L"Authorization: OAuth " + widen(token) + L"\r\nX-Yandex-Music-Client: " + music_client + L"\r\n";
    }
    if (!body.empty()) {
        headers += json_body ? L"Content-Type: application/json\r\n"
                             : L"Content-Type: application/x-www-form-urlencoded\r\n";
    }
    auto size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(call.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                            size ? const_cast<char*>(body.data()) : nullptr, size, size, 0) ||
        !WinHttpReceiveResponse(call.get(), nullptr)) {
        system_error("Запрос к Яндексу");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(call.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
        system_error("HTTP статус");
    }
    std::string response;
    char chunk[8192];
    DWORD read = 0;
    do {
        if (!WinHttpReadData(call.get(), chunk, sizeof(chunk), &read)) system_error("Чтение HTTP");
        response.append(chunk, read);
        if (response.size() > 16 * 1024 * 1024) throw std::runtime_error("Слишком большой ответ Яндекса");
    } while (read);

    Json root(cJSON_ParseWithLengthOpts(response.c_str(), response.size() + 1, nullptr, true), cJSON_Delete);
    if (status < 200 || status >= 300) {
        auto oauth_error = value(field(root.get(), "error"));
        if (oauth_error.size() > 64 || oauth_error.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") != std::string::npos) {
            oauth_error.clear();
        }
        throw HttpError(status, std::move(oauth_error));
    }
    if (!root) throw std::runtime_error("Яндекс вернул ответ в неизвестном формате");
    return root;
}

const cJSON* result(const Json& root) {
    auto node = field(root.get(), "result");
    if (!node) throw std::runtime_error("В ответе Яндекса отсутствует result");
    return node;
}

std::string track_likes_path(const std::string& uid) {
    return "/users/" + uid + "/likes/tracks";
}

const cJSON* liked_items(const Json& document) {
    auto items = field(field(result(document), "library"), "tracks");
    if (!cJSON_IsArray(items)) throw std::runtime_error("Яндекс не вернул список любимых треков");
    return items;
}

Track parse_track(const cJSON* node) {
    auto id = value(field(node, "id"));
    auto title = value(field(node, "title"));
    if (id.empty() || title.empty() || cJSON_IsFalse(field(node, "available"))) return {};
    std::string names;
    const cJSON* artist;
    cJSON_ArrayForEach(artist, field(node, "artists")) {
        auto name = value(field(artist, "name"));
        if (name.empty()) continue;
        if (!names.empty()) names += ", ";
        names += name;
    }
    return {std::move(id), widen(title), widen(names)};
}

void add_track(std::vector<Track>& tracks, const cJSON* node) {
    auto track = parse_track(node);
    if (!track.id.empty()) tracks.push_back(std::move(track));
}

void add_playlist(std::vector<Playlist>& lists, const cJSON* node, std::string owner) {
    auto kind = value(field(node, "kind"));
    auto name = value(field(node, "title"));
    auto uid = value(field(field(node, "owner"), "uid"));
    if (!uid.empty()) owner = std::move(uid);
    if (!digits(kind) || !digits(owner) || name.empty()) return;
    if (std::none_of(lists.begin(), lists.end(), [&](const Playlist& list) {
        return list.owner == owner && list.kind == kind;
    })) {
        lists.push_back({std::move(owner), std::move(kind), widen(name)});
    }
}

std::string timestamp() {
    using namespace std::chrono;
    return std::to_string(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

std::string feedback_time() {
    SYSTEMTIME utc;
    GetSystemTime(&utc);
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  unsigned(utc.wYear), unsigned(utc.wMonth), unsigned(utc.wDay), unsigned(utc.wHour),
                  unsigned(utc.wMinute), unsigned(utc.wSecond), unsigned(utc.wMilliseconds));
    return text;
}

std::string signature(std::string_view message) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) {
        throw std::runtime_error("Не удалось открыть HMAC-SHA256");
    }
    BYTE digest[32];
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(signing_key)), sizeof(signing_key) - 1, 0);
    if (status >= 0) {
        status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())),
                                static_cast<ULONG>(message.size()), 0);
        if (status >= 0) status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Не удалось подписать запрос");
    char base64[48];
    DWORD size = sizeof(base64);
    if (!CryptBinaryToStringA(digest, sizeof(digest), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, base64, &size)) {
        system_error("Base64");
    }
    std::string encoded(base64);
    if (encoded.ends_with('=')) encoded.pop_back();
    return encoded;
}

std::string device_id() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Не удалось создать идентификатор устройства");
    wchar_t buffer[39];
    if (!StringFromGUID2(guid, buffer, 39)) throw std::runtime_error("Ошибка идентификатора устройства");
    return narrow(std::wstring_view(buffer + 1, 36));
}

int positive_int(const cJSON* node, int fallback, int maximum) {
    if (!cJSON_IsNumber(node) || node->valuedouble < 1 || node->valuedouble > maximum) return fallback;
    return node->valueint;
}
}

std::wstring widen(std::string_view input) {
    if (input.empty()) return {};
    auto bytes = static_cast<int>(input.size());
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), bytes, nullptr, 0);
    if (!len) system_error("UTF-8");
    std::wstring text(len, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), bytes, text.data(), len);
    return text;
}

std::string narrow(std::wstring_view input) {
    if (input.empty()) return {};
    auto chars = static_cast<int>(input.size());
    int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), chars, nullptr, 0, nullptr, nullptr);
    if (!len) system_error("UTF-16");
    std::string text(len, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), chars, text.data(), len, nullptr, nullptr);
    return text;
}

std::string saved_token() {
    PCREDENTIALW raw = nullptr;
    if (!CredReadW(credential_name, CRED_TYPE_GENERIC, 0, &raw)) {
        auto code = GetLastError();
        if (code == ERROR_NOT_FOUND) return {};
        system_error("Диспетчер учётных данных", code);
    }
    std::unique_ptr<CREDENTIALW, decltype(&CredFree)> credential(raw, CredFree);
    if (!credential->CredentialBlobSize) return {};
    return {reinterpret_cast<char*>(credential->CredentialBlob), credential->CredentialBlobSize};
}

void save_token(const std::string& token) {
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(credential_name);
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(token.data()));
    credential.CredentialBlobSize = static_cast<DWORD>(token.size());
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) system_error("Сохранение входа");
}

void forget_token() {
    if (!CredDeleteW(credential_name, CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
        system_error("Удаление сохранённого входа");
    }
}

HttpError::HttpError(DWORD code, std::string message)
    : std::runtime_error(message.empty() ? "Яндекс вернул HTTP " + std::to_string(code) : message), status(code) {}

Music::Music(std::string token) : token_(std::move(token)) {
    if (token_.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("Некорректный OAuth-токен");
}

std::string Music::sign_in(std::stop_token stop, const std::function<void(DeviceCode)>& notify) {
    auto response = request(oauth_host, "/device/code", {}, std::string("client_id=") + oauth_id +
                            "&device_id=" + encode(device_id()) + "&device_name=LiteMusic");
    auto code = value(field(response.get(), "device_code"));
    auto user_code = value(field(response.get(), "user_code"));
    auto url = value(field(response.get(), "verification_url"));
    auto interval = std::chrono::seconds(positive_int(field(response.get(), "interval"), 5, 300));
    auto expires = std::chrono::seconds(positive_int(field(response.get(), "expires_in"), 300, 3600));
    if (code.empty() || user_code.empty() ||
        !(url.starts_with("https://yandex.ru/") || url.starts_with("https://ya.ru/") ||
          url.starts_with("https://oauth.yandex.ru/") || url.starts_with("https://id.yandex.ru/") ||
          url.starts_with("https://passport.yandex.ru/"))) {
        throw std::runtime_error("Яндекс вернул некорректный код входа");
    }
    if (stop.stop_requested()) return {};
    notify({widen(url), widen(user_code)});
    auto deadline = std::chrono::steady_clock::now() + expires;
    std::condition_variable_any wait;
    std::mutex mutex;
    std::unique_lock lock(mutex);
    std::string payload = std::string("grant_type=device_code&code=") + encode(code) +
                          "&client_id=" + oauth_id + "&client_secret=" + oauth_key;
    while (!stop.stop_requested()) {
        auto next = std::min(deadline, std::chrono::steady_clock::now() + interval);
        wait.wait_until(lock, stop, next, [] { return false; });
        if (stop.stop_requested()) return {};
        if (std::chrono::steady_clock::now() >= deadline) break;
        try {
            auto token_json = request(oauth_host, "/token", {}, payload);
            auto token = value(field(token_json.get(), "access_token"));
            if (token.empty()) throw std::runtime_error("Яндекс не вернул токен");
            return token;
        } catch (const HttpError& error) {
            auto reason = std::string_view(error.what());
            if (reason == "slow_down") interval += 5s;
            else if (reason != "authorization_pending") throw;
        }
    }
    if (stop.stop_requested()) return {};
    throw std::runtime_error("Код входа истёк. Начните вход ещё раз");
}

Profile Music::profile() const {
    auto document = request(api_host, "/account/status", token_);
    auto account = field(result(document), "account");
    auto uid = value(field(account, "uid"));
    auto name = value(field(account, "displayName"));
    if (name.empty()) name = value(field(account, "login"));
    if (!digits(uid)) throw std::runtime_error("Не удалось определить аккаунт");
    return {std::move(uid), widen(name)};
}

TracksPage Music::search(const std::wstring& query, size_t page) const {
    auto document = request(api_host, "/search?type=track&nocorrect=false&page=" + std::to_string(page) +
                            "&text=" + encode(narrow(query)), token_);
    auto group = field(result(document), "tracks");
    auto data = field(group, "results");
    TracksPage list;
    const cJSON* item;
    cJSON_ArrayForEach(item, data) add_track(list.tracks, item);
    auto per_page = field(group, "perPage");
    auto total = field(group, "total");
    list.more = cJSON_IsNumber(total) && cJSON_IsNumber(per_page) && per_page->valueint > 0 &&
                (page + 1) * static_cast<size_t>(per_page->valueint) < total->valuedouble;
    return list;
}

TracksPage Music::favorites(const std::string& uid, size_t page) const {
    auto document = request(api_host, track_likes_path(uid) + "?if-modified-since-revision=0", token_);
    auto items = liked_items(document);
    size_t from = page * page_size;
    size_t to = std::min(from + page_size, static_cast<size_t>(cJSON_GetArraySize(items)));
    std::string body;
    auto item = cJSON_GetArrayItem(items, static_cast<int>(from));
    for (size_t i = from; i < to && item; ++i, item = item->next) {
        auto id = value(field(item, "id"));
        if (!digits(id)) continue;
        auto album = value(field(item, "albumId"));
        if (!body.empty()) body += '&';
        body += "track-ids=" + encode(digits(album) ? id + ":" + album : id);
    }
    TracksPage list{{}, to < static_cast<size_t>(cJSON_GetArraySize(items))};
    document.reset();
    if (!body.empty()) {
        auto details = request(api_host, "/tracks", token_, body);
        const cJSON* track;
        cJSON_ArrayForEach(track, result(details)) add_track(list.tracks, track);
    }
    return list;
}

std::vector<std::string> Music::liked_tracks(const std::string& uid) const {
    auto document = request(api_host, track_likes_path(uid) + "?if-modified-since-revision=0", token_);
    std::vector<std::string> ids;
    const cJSON* item;
    cJSON_ArrayForEach(item, liked_items(document)) {
        auto id = value(field(item, "id"));
        if (!id.empty()) ids.push_back(std::move(id));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void Music::set_track_like(const std::string& uid, const std::string& track, bool liked) const {
    auto document = request(api_host, track_likes_path(uid) + (liked ? "/add-multiple" : "/remove"),
                            token_, "track-ids=" + encode(track));
    if (!field(result(document), "revision")) throw std::runtime_error("Яндекс не подтвердил изменение лайка");
}

std::vector<Playlist> Music::playlists(const std::string& uid) const {
    auto path = "/users/" + uid;
    auto own = request(api_host, path + "/playlists/list", token_);
    std::vector<Playlist> lists;
    const cJSON* item;
    cJSON_ArrayForEach(item, result(own)) add_playlist(lists, item, uid);
    own.reset();
    auto likes = request(api_host, path + "/likes/playlists", token_);
    cJSON_ArrayForEach(item, result(likes)) {
        auto owner = value(field(item, "uid"));
        add_playlist(lists, field(item, "playlist"), std::move(owner));
    }
    return lists;
}

TracksPage Music::playlist(const Playlist& list, size_t page) const {
    auto document = request(api_host, "/users/" + list.owner + "/playlists/" + list.kind, token_);
    auto items = field(result(document), "tracks");
    TracksPage tracks;
    size_t from = page * page_size;
    size_t total = static_cast<size_t>(cJSON_GetArraySize(items));
    size_t to = std::min(from + page_size, total);
    auto item = cJSON_GetArrayItem(items, static_cast<int>(from));
    for (size_t i = from; i < to && item; ++i, item = item->next) add_track(tracks.tracks, field(item, "track"));
    tracks.more = to < total;
    return tracks;
}

Audio Music::stream(const std::string& track) const {
    auto ts = timestamp();
    std::string variant = "nq";
    std::string codecs = "mp3";
    auto sign = signature(ts + track + variant + codecs + "raw");
    Json document(nullptr, cJSON_Delete);
    try {
        document = request(api_host, "/get-file-info?ts=" + ts + "&trackId=" + encode(track) +
                           "&quality=" + variant + "&codecs=" + codecs + "&transports=raw&sign=" + encode(sign), token_);
    } catch (const HttpError& error) {
        // The service can reject the signature even with a valid OAuth session.
        if (error.status == 401) throw std::runtime_error("Яндекс отклонил запрос аудио (HTTP 401)");
        throw;
    }
    auto info = field(result(document), "downloadInfo");
    auto url = value(field(info, "url"));
    if (url.empty()) url = value(cJSON_GetArrayItem(field(info, "urls"), 0));
    auto codec = value(field(info, "codec"));
    auto transport = value(field(info, "transport"));
    auto bitrate = value(field(info, "bitrate"));
    if (!url.starts_with("https://") || (!codec.empty() && codec != "mp3") || (!transport.empty() && transport != "raw") ||
        !value(field(info, "key")).empty()) {
        throw std::runtime_error("Яндекс не вернул доступный поток MP3");
    }
    return {widen(url), bitrate.empty() ? L"MP3" : L"MP3 · " + widen(bitrate) + L" кбит/с"};
}

Wave Music::wave(const std::string& previous) const {
    auto path = std::string(station) + "/tracks?settings2=true";
    if (!previous.empty()) path += "&queue=" + encode(previous);
    auto document = request(api_host, path, token_);
    auto data = result(document);
    Wave batch{value(field(data, "batchId")), {}};
    const cJSON* item;
    cJSON_ArrayForEach(item, field(data, "sequence")) add_track(batch.tracks, field(item, "track"));
    if (batch.tracks.empty() || batch.batch.empty()) throw std::runtime_error("Яндекс не вернул треки «Моей волны»");
    return batch;
}

void Music::wave_event(std::string_view type, const std::string& batch, const std::string& track,
                       double played_seconds) const {
    auto path = std::string(station) + "/feedback";
    if (!batch.empty()) path += "?batch-id=" + encode(batch);
    Json payload(cJSON_CreateObject(), cJSON_Delete);
    std::string event(type);
    if (!payload || !cJSON_AddStringToObject(payload.get(), "type", event.c_str()) ||
        !cJSON_AddStringToObject(payload.get(), "timestamp", feedback_time().c_str()) ||
        (type == "radioStarted" && !cJSON_AddStringToObject(payload.get(), "from", "desktop-radio")) ||
        (!track.empty() && !cJSON_AddStringToObject(payload.get(), "trackId", track.c_str())) ||
        (played_seconds > 0 && !cJSON_AddNumberToObject(payload.get(), "totalPlayedSeconds", played_seconds))) {
        throw std::bad_alloc();
    }
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(payload.get()), cJSON_free);
    if (!body) throw std::bad_alloc();
    request(api_host, path, token_, body.get(), true);
}

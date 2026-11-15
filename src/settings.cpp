#include <windows.h>
#include "settings.hpp"
#include "cJSON.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace {
constexpr wchar_t key[] = L"Software\\LiteMusic";
constexpr wchar_t volume_value[] = L"Volume";
constexpr wchar_t resume_value[] = L"LastTrack";
constexpr wchar_t discord_value[] = L"DiscordApplicationId";
constexpr DWORD max_bytes = 64 * 1024;
constexpr LONGLONG max_position = 24LL * 60 * 60 * 10'000'000;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool track_id(std::string_view value) {
    return !value.empty() && value.size() <= 24 && value.find_first_not_of("0123456789") == std::string_view::npos;
}

std::string json_string(const cJSON* object, const char* name) {
    auto member = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(member) ? member->valuestring : "";
}

void write_value(const wchar_t* name, DWORD type, const void* data, DWORD size) {
    HKEY handle{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(handle, name, 0, type, static_cast<const BYTE*>(data), size);
    RegCloseKey(handle);
}
}

int load_volume() {
    DWORD volume{}, size = sizeof(volume);
    if (RegGetValueW(HKEY_CURRENT_USER, key, volume_value, RRF_RT_REG_DWORD, nullptr, &volume, &size) != ERROR_SUCCESS ||
        volume > 100) return 100;
    return static_cast<int>(volume);
}

void save_volume(int volume) {
    auto stored = static_cast<DWORD>(std::clamp(volume, 0, 100));
    write_value(volume_value, REG_DWORD, &stored, sizeof(stored));
}

std::optional<ResumeTrack> load_resume_track() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, resume_value, RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size == 0 || size > max_bytes) return {};
    std::string data(size, '\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key, resume_value, RRF_RT_REG_BINARY, nullptr, data.data(), &size) != ERROR_SUCCESS) return {};
    data.resize(size);
    Json json(cJSON_ParseWithLength(data.c_str(), data.size() + 1), cJSON_Delete);
    auto uid = json_string(json.get(), "account");
    auto id = json_string(json.get(), "track");
    auto title = json_string(json.get(), "title");
    auto artist = json_string(json.get(), "artist");
    auto position = cJSON_GetObjectItemCaseSensitive(json.get(), "position");
    if (!track_id(id) || !track_id(uid) || title.empty() || !cJSON_IsNumber(position) ||
        !std::isfinite(position->valuedouble) || position->valuedouble < 0 ||
        position->valuedouble > static_cast<double>(max_position)) return {};
    return ResumeTrack{std::move(uid), {std::move(id), widen(title), widen(artist)},
                       static_cast<LONGLONG>(position->valuedouble)};
}

void save_resume_track(const ResumeTrack& resume) {
    Json json(cJSON_CreateObject(), cJSON_Delete);
    if (!json || !cJSON_AddStringToObject(json.get(), "account", resume.account.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "track", resume.track.id.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "title", narrow(resume.track.name).c_str()) ||
        !cJSON_AddStringToObject(json.get(), "artist", narrow(resume.track.artists).c_str()) ||
        !cJSON_AddNumberToObject(json.get(), "position", static_cast<double>(resume.position))) return;
    auto data = cJSON_PrintUnformatted(json.get());
    if (!data) return;
    auto size = std::char_traits<char>::length(data);
    if (size <= max_bytes) write_value(resume_value, REG_BINARY, data, static_cast<DWORD>(size));
    cJSON_free(data);
}

void erase_resume_track() {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, key, resume_value);
}

std::string load_discord_id() {
    wchar_t id[21]{};
    DWORD size = sizeof(id);
    if (RegGetValueW(HKEY_CURRENT_USER, key, discord_value, RRF_RT_REG_SZ, nullptr, id, &size) != ERROR_SUCCESS) return {};
    return narrow(id);
}

void save_discord_id(const std::string& id) {
    auto stored = widen(id);
    write_value(discord_value, REG_SZ, stored.c_str(), static_cast<DWORD>((stored.size() + 1) * sizeof(wchar_t)));
}

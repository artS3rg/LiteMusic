#include "settings.hpp"
#include "cJSON.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

fs::path directory() {
#ifndef __APPLE__
    if (auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && fs::path(xdg).is_absolute()) {
        return fs::path(xdg) / "litemusic";
    }
#endif
    auto* home = std::getenv("HOME");
    if (!home || !*home) return {};
#ifdef __APPLE__
    return fs::path(home) / "Library/Application Support/LiteMusic";
#else
    return fs::path(home) / ".config/litemusic";
#endif
}

std::string read(const char* file) {
    auto dir = directory();
    if (dir.empty()) return {};
    std::ifstream input(dir / file, std::ios::binary);
    if (!input) return {};
    std::string text(64 * 1024, '\0');
    input.read(text.data(), text.size());
    if (!input.eof()) return {};
    text.resize(input.gcount());
    return text;
}

void write(const char* file, std::string_view text) {
    auto dir = directory();
    if (dir.empty()) return;
    std::error_code error;
    fs::create_directories(dir, error);
    if (error) return;
    auto temp = dir / (std::string(file) + "." + std::to_string(getpid()) + ".tmp");
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    output.write(text.data(), text.size());
    output.close();
    if (output) fs::rename(temp, dir / file, error);
    fs::remove(temp, error);
}

const char* string(const cJSON* node, const char* key) {
    auto* value = cJSON_GetObjectItemCaseSensitive(node, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}
}

int load_volume() {
    auto value = read("volume");
    if (value.empty() || value.size() > 3 || value.find_first_not_of("0123456789") != std::string::npos) return 100;
    return std::clamp(std::stoi(value), 0, 100);
}
void save_volume(int volume) { write("volume", std::to_string(volume)); }

std::optional<ResumeTrack> load_resume_track() {
    auto data = read("last-track.json");
    Json json(cJSON_ParseWithLength(data.c_str(), data.size() + 1), cJSON_Delete);
    std::string id = string(json.get(), "track");
    std::string account = string(json.get(), "account");
    auto* value = cJSON_GetObjectItemCaseSensitive(json.get(), "position");
    if (id.empty() || id.size() > 24 || id.find_first_not_of("0123456789") != std::string::npos ||
        account.empty() || account.find_first_not_of("0123456789") != std::string::npos ||
        !cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > 864000000000.0) return {};
    return ResumeTrack{account, {id, widen(string(json.get(), "name")), widen(string(json.get(), "artists"))},
                       static_cast<std::int64_t>(value->valuedouble)};
}

void save_resume_track(const ResumeTrack& resume) {
    Json json(cJSON_CreateObject(), cJSON_Delete);
    if (!json || !cJSON_AddStringToObject(json.get(), "account", resume.account.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "track", resume.track.id.c_str()) ||
        !cJSON_AddStringToObject(json.get(), "name", narrow(resume.track.name).c_str()) ||
        !cJSON_AddStringToObject(json.get(), "artists", narrow(resume.track.artists).c_str()) ||
        !cJSON_AddNumberToObject(json.get(), "position", static_cast<double>(resume.position))) return;
    std::unique_ptr<char, decltype(&cJSON_free)> data(cJSON_PrintUnformatted(json.get()), cJSON_free);
    if (data) write("last-track.json", data.get());
}

void erase_resume_track() {
    auto dir = directory();
    if (dir.empty()) return;
    std::error_code error;
    fs::remove(dir / "last-track.json", error);
}

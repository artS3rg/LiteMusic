#include "music_platform.hpp"

#include <curl/curl.h>
#include <uuid/uuid.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonHMAC.h>
#include <Security/Security.h>
#else
#include <libsecret/secret.h>
#include <openssl/hmac.h>
#endif

#include <array>
#include <chrono>
#include <codecvt>
#include <cstdio>
#include <ctime>
#include <locale>
#include <memory>
#include <utility>

namespace {
constexpr char service[] = "LiteMusic/YandexMusic";
constexpr char account[] = "OAuth";

#ifndef __APPLE__
const SecretSchema schema = {
    "io.github.artS3rg.LiteMusic", SECRET_SCHEMA_NONE,
    {{"account", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}
};

void check_secret(GError* error) {
    if (!error) return;
    g_error_free(error);
    throw std::runtime_error("Не удалось открыть системное хранилище паролей");
}
#else
void check_secret(OSStatus status) {
    if (status != errSecSuccess) {
        throw std::runtime_error("Связка ключей macOS: " + std::to_string(status));
    }
}
#endif

struct Headers {
    curl_slist* list = nullptr;
    ~Headers() { curl_slist_free_all(list); }
    void add(const char* value) {
        auto* next = curl_slist_append(list, value);
        if (!next) throw std::bad_alloc();
        list = next;
    }
};

size_t append_response(char* data, size_t size, size_t count, void* output) noexcept {
    auto& body = *static_cast<std::string*>(output);
    const auto length = size * count;
    if (length > 16 * 1024 * 1024 - body.size()) return 0;
    try { body.append(data, length); }
    catch (...) { return 0; }
    return length;
}
}

HttpResponse https_request(const char* host, std::string_view path, std::string_view token,
                           std::string_view body, bool json_body, unsigned short port, bool secure, long timeout_ms) {
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw std::runtime_error("Не удалось запустить HTTP");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> request(curl_easy_init(), curl_easy_cleanup);
    if (!request) throw std::runtime_error("Не удалось создать HTTP-запрос");
    auto* handle = request.get();
    auto url = std::string(secure ? "https://" : "http://") + host + ":" + std::to_string(port) + std::string(path);
    Headers headers;
    headers.add("Accept: application/json");
    headers.add("Accept-Language: ru");
    if (!token.empty()) {
        headers.add((std::string("Authorization: OAuth ") + std::string(token)).c_str());
        headers.add("X-Yandex-Music-Client: YandexMusicDesktopAppWindows/5.95.0");
    }
    if (!body.empty()) {
        headers.add(json_body ? "Content-Type: application/json" : "Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    std::string response;
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "LiteMusic/0.1");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 6000L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 15000L);
    if (!secure) curl_easy_setopt(handle, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
    auto result = curl_easy_perform(handle);
    if (result != CURLE_OK) throw std::runtime_error(std::string("Запрос HTTPS: ") + curl_easy_strerror(result));
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    return {static_cast<unsigned long>(status), std::move(response)};
}

std::string signed_hash(std::string_view key, std::string_view message) {
    std::array<unsigned char, 32> digest{};
#ifdef __APPLE__
    CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), message.data(), message.size(), digest.data());
#else
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(message.data()), message.size(), digest.data(), nullptr)) {
        throw std::runtime_error("Не удалось подписать запрос");
    }
#endif
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned bits = 0, buffer = 0;
    for (auto byte : digest) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += alphabet[(buffer >> bits) & 63];
        }
    }
    if (bits) out += alphabet[(buffer << (6 - bits)) & 63];
    return out;
}

std::string device_id() {
    uuid_t uuid;
    uuid_generate_random(uuid);
    char value[37];
    uuid_unparse_lower(uuid, value);
    return value;
}

std::string feedback_time() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto seconds = floor<std::chrono::seconds>(now);
    const auto raw = system_clock::to_time_t(seconds);
    std::tm utc{};
    gmtime_r(&raw, &utc);
    char date[24], stamp[32];
    std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
    std::snprintf(stamp, sizeof(stamp), "%s.%03dZ", date, int(duration_cast<milliseconds>(now - seconds).count()));
    return stamp;
}

std::wstring widen(std::string_view value) {
    if (value.empty()) return {};
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(value.data(), value.data() + value.size());
}

std::string narrow(std::wstring_view value) {
    if (value.empty()) return {};
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>().to_bytes(value.data(), value.data() + value.size());
}

std::string saved_token() {
#ifdef __APPLE__
    void* password = nullptr;
    UInt32 length = 0;
    auto status = SecKeychainFindGenericPassword(nullptr, sizeof(service) - 1, service, sizeof(account) - 1, account,
                                                 &length, &password, nullptr);
    if (status == errSecItemNotFound) return {};
    check_secret(status);
    auto free_password = [](void* ptr) { SecKeychainItemFreeContent(nullptr, ptr); };
    std::unique_ptr<void, decltype(free_password)> saved(password, free_password);
    return length ? std::string(static_cast<char*>(password), length) : std::string();
#else
    GError* error = nullptr;
    std::unique_ptr<gchar, decltype(&secret_password_free)> saved(
        secret_password_lookup_sync(&schema, nullptr, &error, "account", account, nullptr), secret_password_free);
    check_secret(error);
    return saved ? saved.get() : "";
#endif
}

void save_token(const std::string& token) {
#ifdef __APPLE__
    SecKeychainItemRef item = nullptr;
    auto status = SecKeychainFindGenericPassword(nullptr, sizeof(service) - 1, service, sizeof(account) - 1, account,
                                                 nullptr, nullptr, &item);
    if (status == errSecItemNotFound) {
        check_secret(SecKeychainAddGenericPassword(nullptr, sizeof(service) - 1, service, sizeof(account) - 1, account,
                                                   static_cast<UInt32>(token.size()), token.data(), nullptr));
        return;
    }
    check_secret(status);
    status = SecKeychainItemModifyAttributesAndData(item, nullptr, static_cast<UInt32>(token.size()), token.data());
    CFRelease(item);
    check_secret(status);
#else
    GError* error = nullptr;
    const auto saved = secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, service, token.c_str(),
                                                   nullptr, &error, "account", account, nullptr);
    check_secret(error);
    if (!saved) throw std::runtime_error("Системное хранилище паролей недоступно");
#endif
}

void forget_token() {
#ifdef __APPLE__
    SecKeychainItemRef item = nullptr;
    auto status = SecKeychainFindGenericPassword(nullptr, sizeof(service) - 1, service, sizeof(account) - 1, account,
                                                 nullptr, nullptr, &item);
    if (status == errSecItemNotFound) return;
    check_secret(status);
    status = SecKeychainItemDelete(item);
    CFRelease(item);
    check_secret(status);
#else
    GError* error = nullptr;
    secret_password_clear_sync(&schema, nullptr, &error, "account", account, nullptr);
    check_secret(error);
#endif
}

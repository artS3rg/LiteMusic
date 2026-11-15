#include <windows.h>
#include "music_platform.hpp"

#include <bcrypt.h>
#include <wincred.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <objbase.h>

#include <cstdio>
#include <memory>
#include <utility>

namespace {
constexpr wchar_t credential_name[] = L"LiteMusic/YandexMusic";
constexpr wchar_t music_client[] = L"YandexMusicDesktopAppWindows/5.95.0";
using Internet = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

[[noreturn]] void system_error(const char* action, DWORD code = GetLastError()) {
    throw std::runtime_error(std::string(action) + " (Windows " + std::to_string(code) + ")");
}

}

HttpResponse https_request(const char* host, std::string_view path, std::string_view token,
                           std::string_view body, bool json_body, unsigned short port, bool secure, long timeout_ms) {
    Internet session(WinHttpOpen(L"LiteMusic/0.1", secure ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
    if (!session) system_error("WinHTTP");
    if (timeout_ms > 0) {
        int timeout = static_cast<int>(timeout_ms);
        WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);
    } else {
        WinHttpSetTimeouts(session.get(), 4000, 5000, 6000, 8000);
    }
    Internet connection(WinHttpConnect(session.get(), widen(host).c_str(), port, 0), WinHttpCloseHandle);
    if (!connection) system_error("WinHttpConnect");
    auto target = widen(path);
    Internet call(WinHttpOpenRequest(connection.get(), body.empty() ? L"GET" : L"POST", target.c_str(),
                                    nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0),
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
        system_error("Запрос HTTPS");
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

    return {status, std::move(response)};
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

std::string signed_hash(std::string_view key, std::string_view message) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) {
        throw std::runtime_error("Не удалось открыть HMAC-SHA256");
    }
    BYTE digest[32];
    BCRYPT_HASH_HANDLE hash = nullptr;
    auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), static_cast<ULONG>(key.size()), 0);
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

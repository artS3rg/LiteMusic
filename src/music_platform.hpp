#pragma once

#include "music.hpp"

struct HttpResponse {
    unsigned long status;
    std::string body;
};

HttpResponse https_request(const char* host, std::string_view path, std::string_view token,
                           std::string_view body, bool json_body, unsigned short port = 443,
                           bool secure = true, long timeout_ms = 0);
std::string signed_hash(std::string_view key, std::string_view message);
std::string device_id();
std::string feedback_time();

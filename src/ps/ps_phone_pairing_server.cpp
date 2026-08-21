#ifdef __SWITCH__

#include "ps_phone_pairing_server.h"
#include "ps_pairing_account.h"
#include "../diagnostics.h"

#include <switch.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>

namespace lunar::ps {
namespace {

constexpr size_t kMaxRequestSize = 8192;
constexpr size_t kMaxBodySize = 1024;

struct PairingText {
    const char* lang;
    const char* title;
    const char* detail;
    const char* account;
    const char* account_hint;
    const char* account_type;
    const char* base64_option;
    const char* hex_option;
    const char* decimal_option;
    const char* lookup_instruction;
    const char* lookup_link;
    const char* format_hint;
    const char* pin;
    const char* submit;
    const char* privacy;
    const char* sent;
    const char* invalid;
    const char* invalid_base64;
    const char* invalid_pin;
};

const PairingText& pairingText(const std::string& locale) {
    static const PairingText english{
        "en", "LunarNX local pairing",
        "Paste the console user's Base64 Account ID, then enter the 8-digit PIN shown by the console.",
        "Base64 Account ID", "Paste the Encoded ID, for example AbCdEf12345=",
        "Account ID format", "Base64 Account ID", "Apollo Hex (little-endian)",
        "Decimal Account ID",
        "Choose the format shown by your local pairing tool. No online lookup is used.", "",
        "Base64 decodes to 8 bytes. Apollo Hex is converted locally in little-endian order.", "8-digit PIN",
        "Send to Switch", "Account ID and PIN are sent only to LunarNX on your local network.",
        "Sent. Check your Switch for the pairing result.",
        "Invalid Account ID or PIN.",
        "Invalid Base64 Account ID. It must decode to exactly 8 bytes.",
        "PIN must contain exactly 8 digits."
    };
    static const PairingText simplified{
        "zh-CN", "LunarNX 本地配对",
        "请粘贴主机用户的 Base64 Account ID，然后输入主机画面显示的 8 位 PIN 码。",
        "Base64 Account ID", "粘贴 Encoded ID，例如 AbCdEf12345=",
        "Account ID 格式", "Base64 Account ID", "Apollo 十六进制（小端序）",
        "十进制 Account ID",
        "请选择离线工具显示的格式。不会进行在线查询。", "",
        "Base64 解码后必须为 8 字节；Apollo 十六进制会在本机按小端序转换。", "8 位 PIN 码",
        "发送到 Switch", "Account ID 和 PIN 只会通过当前局域网发送给 LunarNX。",
        "已发送，请查看 Switch 上的配对结果。",
        "Account ID 或 PIN 无效。",
        "Base64 Account ID 无效。解码后必须正好为 8 字节。",
        "PIN 必须正好包含 8 位数字。"
    };
    static const PairingText traditional{
        "zh-TW", "LunarNX 本機配對",
        "請貼上主機使用者的 Base64 Account ID，然後輸入主機畫面顯示的 8 位 PIN 碼。",
        "Base64 Account ID", "貼上 Encoded ID，例如 AbCdEf12345=",
        "Account ID 格式", "Base64 Account ID", "Apollo 十六進位（小端序）",
        "十進位 Account ID",
        "請選擇離線工具顯示的格式。不會進行線上查詢。", "",
        "Base64 解碼後必須為 8 位元組；Apollo 十六進位會在本機按小端序轉換。", "8 位 PIN 碼",
        "傳送至 Switch", "Account ID 和 PIN 只會透過目前區域網路傳送給 LunarNX。",
        "已傳送，請查看 Switch 上的配對結果。",
        "Account ID 或 PIN 無效。",
        "Base64 Account ID 無效。解碼後必須正好為 8 位元組。",
        "PIN 必須正好包含 8 位數字。"
    };
    if (locale == "zh-Hans") return simplified;
    if (locale == "zh-Hant") return traditional;
    return english;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool formDecode(const std::string& value, std::string& decoded) {
    decoded.clear();
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') decoded.push_back(' ');
        else if (value[i] == '%') {
            if (i + 2 >= value.size()) return false;
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high < 0 || low < 0) return false;
            decoded.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else decoded.push_back(value[i]);
        if (decoded.size() > 256) return false;
    }
    return true;
}

bool formValue(const std::string& body, const std::string& key, std::string& value) {
    const std::string prefix = key + "=";
    size_t start = body.find(prefix);
    if (start == std::string::npos || (start != 0 && body[start - 1] != '&')) return false;
    start += prefix.size();
    const size_t end = body.find('&', start);
    return formDecode(body.substr(start, end == std::string::npos
        ? std::string::npos : end - start), value);
}

std::string sessionToken() {
    unsigned char bytes[16]{};
    randomGet(bytes, sizeof(bytes));
    char token[sizeof(bytes) * 2 + 1]{};
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        std::snprintf(token + i * 2, 3, "%02x", bytes[i]);
    }
    return token;
}

std::string pairingPage(const std::string& submit_path, const PairingText& text) {
    return "<!doctype html><html lang=\"" + std::string(text.lang) +
        "\"><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>" + text.title + "</title><style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;background:#f4f5f7;color:#17191c}"
        "main{max-width:520px;margin:auto;padding:30px 20px}h1{font-size:25px}p{line-height:1.55}"
        "label{display:block;font-weight:600;margin:20px 0 7px}input,button{box-sizing:border-box;width:100%;padding:14px;"
        "border-radius:7px;font-size:17px}input{border:1px solid #aeb4bd;background:white}button{margin-top:24px;border:0;background:#1769e0;color:white;font-weight:600}"
        "a{color:#1769e0;font-weight:600}"
        ".note{color:#59616d;font-size:13px}</style></head><body><main><h1>" + text.title +
        "</h1><p>" + text.detail + "</p><form method=post action=\"" + submit_path +
        "\"><p>" + text.lookup_instruction + "</p>"
        "<label>" + text.account_type + "</label><select name=\"account_type\" required>"
        "<option value=\"base64_id\">" + text.base64_option + "</option>"
        "<option value=\"hex_id\">" + text.hex_option + "</option>"
        "<option value=\"decimal_id\">" + text.decimal_option + "</option></select>"
        "<label>" + text.account + "</label><input name=\"account_input\" required autocomplete=off autocapitalize=off spellcheck=false placeholder=\"" +
        text.account_hint + "\"><p class=note>" + text.format_hint + "</p><label>" + text.pin +
        "</label><input name=\"pin\" type=\"password\" required inputmode=\"numeric\" pattern=\"[0-9]{8}\" maxlength=\"8\">"
        "<button type=submit>" + text.submit + "</button></form><p class=note>" + text.privacy +
        "</p></main></body></html>";
}

std::string resultPage(bool ok, const PairingText& text,
                       const char* error = nullptr) {
    const char* message = ok ? text.sent : (error ? error : text.invalid);
    return "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<body style=\"font-family:sans-serif;padding:32px\"><h2>" + std::string(ok ? text.submit : text.title) +
        "</h2><p>" + message + "</p></body>";
}

bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = send(fd, data.data() + sent, data.size() - sent, 0);
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

void sendResponse(int fd, int status, const std::string& body) {
    const char* status_text = status == 200 ? "200 OK" :
        status == 400 ? "400 Bad Request" : "404 Not Found";
    const std::string response = "HTTP/1.1 " + std::string(status_text) +
        "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    sendAll(fd, response);
}

bool readRequest(int fd, std::string& request) {
    request.clear();
    char buffer[2048];
    size_t expected = 0;
    while (request.size() < kMaxRequestSize) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) return false;
        request.append(buffer, static_cast<size_t>(count));
        const size_t header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) continue;
        if (expected == 0) {
            size_t content_length = 0;
            const size_t field = request.find("Content-Length:");
            if (field != std::string::npos && field < header_end) {
                content_length = std::strtoul(request.c_str() + field + 15, nullptr, 10);
                if (content_length > kMaxBodySize) return false;
            }
            expected = header_end + 4 + content_length;
        }
        if (request.size() >= expected) return true;
    }
    return false;
}

} // namespace

PsPhonePairingServer::~PsPhonePairingServer() { stop(); }

bool PsPhonePairingServer::start(const std::string& locale, Callback callback) {
    stop();
    stopping_.store(false);
    u32 current_address = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&current_address)) || current_address == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Switch has no local network address";
        return false;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(fd, 2) != 0) {
        close(fd);
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Could not start phone pairing service";
        return false;
    }
    socklen_t size = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        close(fd);
        return false;
    }
    in_addr local_address{current_address};
    char ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &local_address, ip, sizeof(ip))) {
        close(fd);
        return false;
    }
    const std::string path = "/pair/" + sessionToken();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        listen_fd_ = fd;
        helper_url_ = "http://" + std::string(ip) + ":" +
            std::to_string(ntohs(address.sin_port)) + path;
    }
    try {
        thread_ = std::thread(&PsPhonePairingServer::run, this, path, locale,
                              std::move(callback));
    } catch (...) {
        close(fd);
        std::lock_guard<std::mutex> lock(mutex_);
        listen_fd_ = -1;
        helper_url_.clear();
        error_ = "Could not start phone pairing worker";
        return false;
    }
    return true;
}

void PsPhonePairingServer::stop() {
    stopping_.store(true);
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fd = listen_fd_;
        listen_fd_ = -1;
        helper_url_.clear();
    }
    if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
}

std::string PsPhonePairingServer::getHelperUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return helper_url_;
}

std::string PsPhonePairingServer::getError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void PsPhonePairingServer::run(std::string session_path, std::string locale,
                               Callback callback) {
    const std::string submit_path = session_path + "/submit";
    const PairingText& text = pairingText(locale);
    const std::string page = pairingPage(submit_path, text);
    bool consumed = false;
    while (!stopping_.load() && !consumed) {
        int listen_fd = -1;
        { std::lock_guard<std::mutex> lock(mutex_); listen_fd = listen_fd_; }
        if (listen_fd < 0) break;
        const int client = accept(listen_fd, nullptr, nullptr);
        if (client < 0) break;
        timeval timeout{}; timeout.tv_sec = 10;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::string request;
        if (!readRequest(client, request)) {
            sendResponse(client, 400, resultPage(false, text));
            close(client);
            continue;
        }
        const std::string line = request.substr(0, request.find("\r\n"));
        if (line == "GET " + session_path + " HTTP/1.1") {
            sendResponse(client, 200, page);
        } else if (line == "POST " + submit_path + " HTTP/1.1") {
            const size_t body_start = request.find("\r\n\r\n");
            const std::string body = body_start == std::string::npos
                ? "" : request.substr(body_start + 4);
            std::string account_type;
            std::string account_input;
            std::string account_id;
            std::string pin_text;
            const bool has_fields = formValue(body, "account_type", account_type) &&
                formValue(body, "account_input", account_input) &&
                formValue(body, "pin", pin_text);
            const bool pin_valid = has_fields && pin_text.size() == 8 &&
                std::all_of(pin_text.begin(), pin_text.end(), [](unsigned char c) {
                    return std::isdigit(c) != 0;
                });
            bool valid = has_fields && pin_valid;
            const char* validation_error = pin_valid ? text.invalid : text.invalid_pin;
            if (valid) {
                if (account_type == "base64_id") {
                    valid = normalizeBase64PsnAccountId(account_input, account_id);
                } else if (account_type == "hex_id") {
                    valid = hexPsnAccountIdToBase64(account_input, account_id);
                } else if (account_type == "decimal_id") {
                    valid = decimalPsnAccountIdToBase64(account_input, account_id);
                } else {
                    valid = false;
                }
                if (!valid) validation_error = text.invalid_base64;
            }
            uint32_t pin = valid ? static_cast<uint32_t>(std::strtoul(pin_text.c_str(), nullptr, 10)) : 0;
            sendResponse(client, valid ? 200 : 400,
                         resultPage(valid, text, validation_error));
            if (valid) {
                consumed = true;
                if (callback) callback(PsPhonePairingInput{std::move(account_id), pin});
            }
        } else sendResponse(client, 404, resultPage(false, text));
        close(client);
    }
    int fd = -1;
    { std::lock_guard<std::mutex> lock(mutex_); fd = listen_fd_; listen_fd_ = -1; }
    if (fd >= 0) close(fd);
    diagnosticLog("ps-phone-pair", "helper stopped consumed=%d", consumed ? 1 : 0);
}

} // namespace lunar::ps

#endif

#ifdef __SWITCH__

#include "psn_callback_server.h"
#include "../diagnostics.h"

#include <switch.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lunar::ps {
namespace {

constexpr size_t kMaxRequestSize = 12 * 1024;
constexpr size_t kMaxCallbackSize = 4096;

struct HelperText {
    const char* lang;
    const char* title;
    const char* heading;
    const char* keep_open;
    const char* step_open;
    const char* step_copy;
    const char* step_return;
    const char* open_button;
    const char* address_label;
    const char* send_button;
    const char* privacy;
    const char* sent_title;
    const char* sent_detail;
    const char* invalid_title;
    const char* invalid_detail;
};

const HelperText& helperText(const std::string& locale) {
    static const HelperText english{
        "en", "LunarNX PSN sign-in", "Sign in to PSN for LunarNX",
        "Keep this page open. The Sony sign-in page will open in another tab.",
        "Tap <b>Open Sony sign-in</b> and finish signing in.",
        "When a page showing <b>redirect</b> appears, copy the complete address from the browser address bar.",
        "Return to this tab, paste the address below, and tap <b>Send to Switch</b>.",
        "Open Sony sign-in", "Sony redirect address", "Send to Switch",
        "Your PSN password and tokens are never sent through this page. The authorization code is delivered directly to LunarNX on your local network.",
        "Sent to LunarNX", "Look at your Switch. LunarNX is completing sign-in now.",
        "Could not read that address", "Go back and paste the complete Sony redirect address."
    };
    static const HelperText simplified{
        "zh-CN", "LunarNX PSN 登录", "登录 PSN 并连接 LunarNX",
        "请保持本页面打开。Sony 登录页面会在新的浏览器标签页中打开。",
        "点击<b>打开 Sony 登录页面</b>并完成登录。",
        "看到内容为 <b>redirect</b> 的页面后，复制浏览器地址栏中的完整地址。",
        "返回本标签页，将地址粘贴到下方，然后点击<b>发送到 Switch</b>。",
        "打开 Sony 登录页面", "Sony 跳转地址", "发送到 Switch",
        "本页面不会接收你的 PSN 密码或 token。授权 code 只会通过当前局域网直接发送到 LunarNX。",
        "已发送到 LunarNX", "请查看 Switch，LunarNX 正在完成登录。",
        "无法识别这个地址", "请返回并粘贴完整的 Sony 跳转地址。"
    };
    static const HelperText traditional{
        "zh-TW", "LunarNX PSN 登入", "登入 PSN 並連線 LunarNX",
        "請保持本頁面開啟。Sony 登入頁面會在新的瀏覽器分頁中開啟。",
        "點選<b>開啟 Sony 登入頁面</b>並完成登入。",
        "看到內容為 <b>redirect</b> 的頁面後，複製瀏覽器網址列中的完整網址。",
        "返回本分頁，將網址貼到下方，然後點選<b>傳送至 Switch</b>。",
        "開啟 Sony 登入頁面", "Sony 重新導向網址", "傳送至 Switch",
        "本頁面不會接收你的 PSN 密碼或 token。授權 code 只會透過目前區域網路直接傳送至 LunarNX。",
        "已傳送至 LunarNX", "請查看 Switch，LunarNX 正在完成登入。",
        "無法辨識這個網址", "請返回並貼上完整的 Sony 重新導向網址。"
    };
    if (locale == "zh-Hans") return simplified;
    if (locale == "zh-Hant") return traditional;
    return english;
}

std::string htmlEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool formDecode(const std::string& value, std::string& decoded) {
    decoded.clear();
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            decoded.push_back(' ');
        } else if (value[i] == '%') {
            if (i + 2 >= value.size()) return false;
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high < 0 || low < 0) return false;
            decoded.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else {
            decoded.push_back(value[i]);
        }
        if (decoded.size() > kMaxCallbackSize) return false;
    }
    return true;
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

std::string helperPage(const std::string& login_url, const std::string& submit_path,
                       const HelperText& text) {
    return "<!doctype html><html lang=\"" + std::string(text.lang) +
        "\"><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>" + text.title + "</title><style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;background:#f4f5f7;color:#17191c}"
        "main{max-width:560px;margin:auto;padding:28px 20px 48px}h1{font-size:25px;margin:0 0 12px}"
        "p,li{font-size:16px;line-height:1.55}ol{padding-left:24px}"
        "a,button{display:block;box-sizing:border-box;width:100%;padding:15px 18px;border:0;border-radius:6px;"
        "font-size:17px;font-weight:600;text-align:center;text-decoration:none;background:#1769e0;color:white}"
        "textarea{box-sizing:border-box;width:100%;min-height:120px;margin:10px 0 14px;padding:12px;"
        "border:1px solid #aeb4bd;border-radius:6px;font-size:15px}"
        ".note{color:#59616d;font-size:14px}.step{margin-top:28px;padding-top:22px;border-top:1px solid #d7dbe0}"
        "</style></head><body><main><h1>" + text.heading + "</h1><p>" +
        text.keep_open + "</p><ol><li>" + text.step_open + "</li><li>" +
        text.step_copy + "</li><li>" + text.step_return + "</li></ol>"
        "<a target=_blank rel=\"noopener noreferrer\" href=\"" + htmlEscape(login_url) +
        "\">" + text.open_button + "</a><section class=step><form method=post action=\"" +
        htmlEscape(submit_path) + "\"><label for=callback><b>" + text.address_label + "</b></label>"
        "<textarea id=callback name=callback required autocomplete=off autocapitalize=off spellcheck=false "
        "placeholder=\"https://remoteplay.dl.playstation.net/remoteplay/redirect?code=...\"></textarea>"
        "<button type=submit>" + text.send_button + "</button></form><p class=note>" +
        text.privacy + "</p></section></main></body></html>";
}

std::string resultPage(bool ok, const HelperText& text) {
    if (ok) {
        return "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
               "<title>" + std::string(text.sent_title) + "</title><body style=\"font-family:sans-serif;padding:32px\">"
               "<h2>" + text.sent_title + "</h2><p>" + text.sent_detail + "</p></body>";
    }
    return "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
           "<title>" + std::string(text.invalid_title) + "</title><body style=\"font-family:sans-serif;padding:32px\">"
           "<h2>" + text.invalid_title + "</h2><p>" + text.invalid_detail + "</p></body>";
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
    std::string response = "HTTP/1.1 " + std::string(status_text) +
        "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\n\r\n" + body;
    sendAll(fd, response);
}

bool readRequest(int fd, std::string& request) {
    request.clear();
    char buffer[2048];
    size_t expected_size = 0;
    while (request.size() < kMaxRequestSize) {
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) return false;
        request.append(buffer, static_cast<size_t>(count));
        const size_t header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) continue;
        if (expected_size == 0) {
            size_t content_length = 0;
            const size_t field = request.find("Content-Length:");
            if (field != std::string::npos && field < header_end) {
                content_length = std::strtoul(request.c_str() + field + 15, nullptr, 10);
                if (content_length > kMaxCallbackSize + 32) return false;
            }
            expected_size = header_end + 4 + content_length;
        }
        if (request.size() >= expected_size) return true;
    }
    return false;
}

} // namespace

PsnCallbackServer::~PsnCallbackServer() {
    stop();
}

bool PsnCallbackServer::start(const std::string& login_url,
                              const std::string& locale, Callback callback) {
    stop();
    stopping_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
    }

    u32 current_address = 0;
    Result rc = nifmGetCurrentIpAddress(&current_address);
    if (R_FAILED(rc) || current_address == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Switch has no local network address";
        return false;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Could not create the phone sign-in service";
        return false;
    }
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
        error_ = "Could not start the phone sign-in service";
        return false;
    }

    socklen_t address_size = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) != 0) {
        close(fd);
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Could not determine the phone sign-in address";
        return false;
    }

    in_addr local_address{current_address};
    char ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &local_address, ip, sizeof(ip))) {
        close(fd);
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Could not determine the Switch network address";
        return false;
    }

    const std::string session_id = sessionToken();
    const std::string session_path = "/psn/" + session_id;
    const std::string session_login_url = login_url + "&state=" + session_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listen_fd_ = fd;
        helper_url_ = "http://" + std::string(ip) + ":" +
            std::to_string(ntohs(address.sin_port)) + session_path;
    }

    try {
        thread_ = std::thread(&PsnCallbackServer::run, this, session_login_url,
                              session_path, locale, std::move(callback));
    } catch (...) {
        close(fd);
        std::lock_guard<std::mutex> lock(mutex_);
        listen_fd_ = -1;
        helper_url_.clear();
        error_ = "Could not start the phone sign-in worker";
        return false;
    }
    diagnosticLog("psn-callback", "phone helper listening port=%u", ntohs(address.sin_port));
    return true;
}

void PsnCallbackServer::stop() {
    stopping_.store(true);
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fd = listen_fd_;
        listen_fd_ = -1;
        helper_url_.clear();
    }
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
        thread_.join();
    }
}

std::string PsnCallbackServer::getHelperUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return helper_url_;
}

std::string PsnCallbackServer::getError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void PsnCallbackServer::run(std::string login_url, std::string session_path,
                            std::string locale, Callback callback) {
    const std::string submit_path = session_path + "/submit";
    const std::string session_id = session_path.substr(session_path.rfind('/') + 1);
    const HelperText& text = helperText(locale);
    const std::string page = helperPage(login_url, submit_path, text);
    bool consumed = false;

    while (!stopping_.load() && !consumed) {
        int listen_fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            listen_fd = listen_fd_;
        }
        if (listen_fd < 0) break;

        const int client = accept(listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (!stopping_.load()) {
                diagnosticLog("psn-callback", "accept failed errno=%d", errno);
            }
            break;
        }

        timeval timeout{};
        timeout.tv_sec = 10;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        std::string request;
        if (!readRequest(client, request)) {
            sendResponse(client, 400, resultPage(false, text));
            close(client);
            continue;
        }

        const size_t line_end = request.find("\r\n");
        const std::string line = request.substr(0, line_end);
        if (line == "GET " + session_path + " HTTP/1.1") {
            sendResponse(client, 200, page);
        } else if (line == "POST " + submit_path + " HTTP/1.1") {
            const size_t body_start = request.find("\r\n\r\n");
            const std::string body = body_start == std::string::npos
                ? std::string{} : request.substr(body_start + 4);
            const std::string prefix = "callback=";
            std::string input;
            const bool valid = body.rfind(prefix, 0) == 0 &&
                formDecode(body.substr(prefix.size()), input) &&
                input.find("https://remoteplay.dl.playstation.net/remoteplay/redirect?") == 0 &&
                input.find("code=") != std::string::npos &&
                (input.find("?state=" + session_id + "&") != std::string::npos ||
                 input.find("&state=" + session_id + "&") != std::string::npos ||
                 input.ends_with("&state=" + session_id));
            sendResponse(client, valid ? 200 : 400, resultPage(valid, text));
            if (valid) {
                consumed = true;
                diagnosticLog("psn-callback", "Sony redirect received length=%zu", input.size());
                if (callback) callback(std::move(input));
            }
        } else {
            sendResponse(client, 404, resultPage(false, text));
        }
        close(client);
    }
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fd = listen_fd_;
        listen_fd_ = -1;
    }
    if (fd >= 0) close(fd);
    diagnosticLog("psn-callback", "phone helper stopped consumed=%d", consumed ? 1 : 0);
}

} // namespace lunar::ps

#endif

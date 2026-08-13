#ifdef __SWITCH__

#include "xbox_login_server.h"
#include "../diagnostics.h"

#include <switch.h>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace lunar::auth {
namespace {

struct HelperText {
    const char* lang;
    const char* title;
    const char* heading;
    const char* intro;
    const char* code_label;
    const char* copy_button;
    const char* copied_button;
    const char* open_button;
    const char* step_finish;
    const char* privacy;
};

const HelperText& helperText(const std::string& locale) {
    static const HelperText english{
        "en", "LunarNX Xbox sign-in", "Sign in to Xbox for LunarNX",
        "Copy this one-time code, then open the Microsoft sign-in page.",
        "Your sign-in code", "Copy code", "Copied", "Open Microsoft sign-in",
        "Paste the code and finish signing in. LunarNX will continue automatically.",
        "Your password and tokens never pass through this page."
    };
    static const HelperText simplified{
        "zh-CN", "LunarNX Xbox 登录", "登录 Xbox 并连接 LunarNX",
        "复制下方的一次性代码，然后打开 Microsoft 登录页面。",
        "你的登录代码", "复制代码", "已复制", "打开 Microsoft 登录页面",
        "粘贴代码并完成登录，LunarNX 会自动继续。",
        "你的密码和 token 不会经过本页面。"
    };
    static const HelperText traditional{
        "zh-TW", "LunarNX Xbox 登入", "登入 Xbox 並連線 LunarNX",
        "複製下方的一次性代碼，然後開啟 Microsoft 登入頁面。",
        "你的登入代碼", "複製代碼", "已複製", "開啟 Microsoft 登入頁面",
        "貼上代碼並完成登入，LunarNX 會自動繼續。",
        "你的密碼和 token 不會經過本頁面。"
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

std::string sessionToken() {
    unsigned char bytes[16]{};
    randomGet(bytes, sizeof(bytes));
    char token[sizeof(bytes) * 2 + 1]{};
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        std::snprintf(token + i * 2, 3, "%02x", bytes[i]);
    }
    return token;
}

std::string helperPage(const std::string& verification_url,
                       const std::string& user_code,
                       const HelperText& text) {
    const std::string code = htmlEscape(user_code);
    return "<!doctype html><html lang=\"" + std::string(text.lang) +
        "\"><head><meta charset=utf-8>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>" + text.title + "</title><style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;background:#f4f5f7;color:#17191c}"
        "main{max-width:560px;margin:auto;padding:30px 20px 48px}h1{font-size:26px;margin:0 0 12px}"
        "p{font-size:16px;line-height:1.55}.code{margin:24px 0 12px;padding:22px 16px;border:1px solid #c8cdd4;"
        "border-radius:6px;background:white;text-align:center;font:700 38px ui-monospace,monospace;letter-spacing:2px}"
        "button,a{display:block;box-sizing:border-box;width:100%;margin-top:12px;padding:15px 18px;border:0;"
        "border-radius:6px;font-size:17px;font-weight:600;text-align:center;text-decoration:none}"
        "button{background:#e4e7eb;color:#17191c}a{background:#107c10;color:white}"
        ".label,.note{color:#59616d;font-size:14px}.finish{margin-top:22px}.note{margin-top:26px}"
        "</style></head><body><main><h1>" + text.heading + "</h1><p>" + text.intro +
        "</p><div class=label>" + text.code_label + "</div><div id=code class=code>" + code +
        "</div><button id=copy type=button>" + text.copy_button + "</button>"
        "<a target=_blank rel=\"noopener noreferrer\" href=\"" + htmlEscape(verification_url) +
        "\">" + text.open_button + "</a><p class=finish>" + text.step_finish +
        "</p><p class=note>" + text.privacy + "</p><script>"
        "const b=document.getElementById('copy'),c=document.getElementById('code').textContent;"
        "b.onclick=async()=>{let ok=false;try{await navigator.clipboard.writeText(c);ok=true}catch(e){}"
        "if(!ok){const t=document.createElement('textarea');t.value=c;document.body.appendChild(t);"
        "t.select();ok=document.execCommand('copy');t.remove()}if(ok)b.textContent='" +
        htmlEscape(text.copied_button) + "'};</script></main></body></html>";
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
    const char* status_text = status == 200 ? "200 OK" : "404 Not Found";
    std::string response = "HTTP/1.1 " + std::string(status_text) +
        "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\n"
        "X-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self' 'unsafe-inline'\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    sendAll(fd, response);
}

} // namespace

XboxLoginServer::~XboxLoginServer() {
    stop();
}

bool XboxLoginServer::start(const std::string& verification_url,
                            const std::string& user_code,
                            const std::string& locale) {
    stop();
    stopping_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
    }
    if (verification_url.empty() || user_code.empty() || user_code.size() > 32) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Invalid Xbox sign-in details";
        return false;
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

    const std::string session_path = "/xbox/" + sessionToken();
    const std::string page = helperPage(
        verification_url, user_code, helperText(locale));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listen_fd_ = fd;
        helper_url_ = "http://" + std::string(ip) + ":" +
            std::to_string(ntohs(address.sin_port)) + session_path;
    }
    try {
        thread_ = std::thread(&XboxLoginServer::run, this, session_path, page);
    } catch (...) {
        close(fd);
        std::lock_guard<std::mutex> lock(mutex_);
        listen_fd_ = -1;
        helper_url_.clear();
        error_ = "Could not start the phone sign-in worker";
        return false;
    }
    diagnosticLog("xbox-login", "phone helper listening port=%u", ntohs(address.sin_port));
    return true;
}

void XboxLoginServer::stop() {
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

std::string XboxLoginServer::getHelperUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return helper_url_;
}

std::string XboxLoginServer::getError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

void XboxLoginServer::run(std::string session_path, std::string page) {
    while (!stopping_.load()) {
        int listen_fd = -1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            listen_fd = listen_fd_;
        }
        if (listen_fd < 0) break;

        pollfd listener{};
        listener.fd = listen_fd;
        listener.events = POLLIN;
        const int ready = poll(&listener, 1, 250);
        if (ready == 0) continue;
        if (ready < 0) {
            if (errno == EINTR) continue;
            if (!stopping_.load()) {
                diagnosticLog("xbox-login", "listener poll failed errno=%d", errno);
            }
            break;
        }
        if ((listener.revents & POLLIN) == 0) {
            if (!stopping_.load() && (listener.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                diagnosticLog("xbox-login", "listener stopped revents=0x%x",
                              listener.revents);
            }
            break;
        }

        const int client = accept(listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (!stopping_.load()) {
                diagnosticLog("xbox-login", "accept failed errno=%d", errno);
            }
            break;
        }
        timeval timeout{};
        timeout.tv_sec = 5;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char request[2048]{};
        const ssize_t count = recv(client, request, sizeof(request) - 1, 0);
        const std::string first_line = count > 0
            ? std::string(request, static_cast<size_t>(count)).substr(
                0, std::string(request, static_cast<size_t>(count)).find("\r\n"))
            : std::string{};
        if (first_line == "GET " + session_path + " HTTP/1.1") {
            sendResponse(client, 200, page);
        } else {
            sendResponse(client, 404, "<!doctype html><title>Not found</title>");
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
    diagnosticLog("xbox-login", "phone helper stopped");
}

} // namespace lunar::auth

#endif

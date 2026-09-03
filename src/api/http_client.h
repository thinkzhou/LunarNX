#pragma once

#include <string>
#include <map>
#include <functional>
#include <mutex>

namespace lunar::api {

struct HttpResponse {
    int status_code = 0;
    bool network_error = false;
    std::string error_message;
    std::string body;
    std::map<std::string, std::string> headers;
};

class HttpClient {
public:
    using CancelCallback = std::function<bool()>;

    HttpClient();
    ~HttpClient();

    HttpResponse get(const std::string& url, const std::map<std::string, std::string>& headers = {},
                     CancelCallback cancel = {});
    HttpResponse getSensitive(const std::string& url, const std::string& diagnostic_url,
                              const std::map<std::string, std::string>& headers = {},
                              CancelCallback cancel = {});
    HttpResponse post(const std::string& url, const std::string& body,
                      const std::map<std::string, std::string>& headers = {},
                      CancelCallback cancel = {});
    HttpResponse del(const std::string& url, const std::map<std::string, std::string>& headers = {},
                     CancelCallback cancel = {});

    void setDefaultHeader(const std::string& key, const std::string& value);

private:
    std::map<std::string, std::string> default_headers_;
    void* curl_handle_ = nullptr;
    std::string body_buf_;
    std::map<std::string, std::string> resp_headers_;
    std::timed_mutex mutex_;

    HttpResponse getImpl(const std::string& url, const std::string& diagnostic_url,
                         const std::map<std::string, std::string>& headers,
                         CancelCallback cancel);
};

} // namespace lunar::api

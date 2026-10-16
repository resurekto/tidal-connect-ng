#include "media/stream/Operation.hpp"

#include <algorithm>
#include <cctype>

#include <curl/curl.h>

#include "util/Log.hpp"
#include "util/HttpClient.hpp"

namespace tcng::media {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

struct Context {
    Operation* op;
    std::atomic<Operation::Status>* status;
    std::vector<char>* body;
    std::map<std::string, std::string>* headers;
};

std::size_t onWrite(char* data, std::size_t size, std::size_t nmemb, void* userp) {
    auto* ctx = static_cast<Context*>(userp);
    ctx->body->insert(ctx->body->end(), data, data + size * nmemb);
    return size * nmemb;
}

std::size_t onHeader(char* data, std::size_t size, std::size_t nmemb, void* userp) {
    auto* ctx = static_cast<Context*>(userp);
    const std::string line(data, size * nmemb);
    const auto colon = line.find(':');
    if (colon != std::string::npos)
        (*ctx->headers)[toLower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    else if (line.rfind("HTTP/", 0) == 0)
        ctx->headers->clear();  // new response (e.g. after a redirect)
    return size * nmemb;
}

int onProgress(void* userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<Context*>(userp);
    return ctx->status->load() == Operation::Status::Aborted ? 1 : 0;
}

}  // namespace

void Operation::abort() {
    status_ = Status::Aborted;
}

void Operation::send(const std::string& url, const std::map<std::string, std::string>& headers, std::size_t reserve) {
    Status expected = Status::Idle;
    if (!status_.compare_exchange_strong(expected, Status::Running))
        return;  // aborted before it started
    body_->clear();
    body_->reserve(reserve);
    responseHeaders_.clear();

    CURL* curl = curl_easy_init();
    if (!curl) {
        status_ = Status::Error;
        statusMessage_ = "curl_easy_init failed";
        return;
    }
    curl_slist* list = nullptr;
    bool hasRange = false;
    for (const auto& [k, v] : headers) {
        list = curl_slist_append(list, (k + ": " + v).c_str());
        hasRange = hasRange || toLower(k) == "range";
    }
    Context ctx{this, &status_, body_.get(), &responseHeaders_};
    char errbuf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &onWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &onProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &onHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Same TLS policy as HttpClient (always verify; --web-ca-path if given).
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    const std::string ca = util::HttpClient::caPath();
    if (!ca.empty())
        curl_easy_setopt(curl, CURLOPT_CAPATH, ca.c_str());
#ifdef _WIN32
    else
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif

    const CURLcode rc = curl_easy_perform(curl);
    Status result = Status::Ok;
    if (rc == CURLE_OK) {
        if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus_) != CURLE_OK) {
            result = Status::Error;
            statusMessage_ = "curl_easy_getinfo failed";
        } else if (hasRange && !responseHeaders_.count("content-range")) {
            // Header names are stored lower-case.
            result = Status::Error;
            statusMessage_ = "Content-Range is missing from response";
        }
    } else if (rc == CURLE_ABORTED_BY_CALLBACK) {
        result = Status::Aborted;
    } else {
        result = rc == CURLE_OPERATION_TIMEDOUT ? Status::Timeout : Status::Error;
        statusMessage_ = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(list);
    expected = Status::Running;
    status_.compare_exchange_strong(expected, result);  // keep Aborted if set meanwhile
}

}  // namespace tcng::media

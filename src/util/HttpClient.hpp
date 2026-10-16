#pragma once
// HttpClient: one blocking libcurl request at a time, with a
// process-wide pool of keep-alive curl handles.
//
// Peer and host verification are always on; with an empty --web-ca-path curl's/the OS
// default CA store is used.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "util/HttpTypes.hpp"

namespace tcng::util {

class HttpClient {
public:
    // curl_global_init(CURL_GLOBAL_ALL); caPath (--web-ca-path) becomes CURLOPT_CAPATH
    // for every request when non-empty. Call once before any request.
    static void globalInit(const std::string& caPath);
    static void globalDeinit();
    // Destroys all pooled keep-alive handles.
    static void globalCloseAllKeepalive();
    // The CA path given to globalInit ("" = curl default store).
    static std::string caPath();

    explicit HttpClient(bool keepAlive);
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    void setTimeout(unsigned seconds);  // CURLOPT_TIMEOUT; 0 (default) = none
    void setUserAgent(const std::string& userAgent);  // default "" = no User-Agent header
    // TCNG always verifies; false is logged and ignored.
    void enableServerVerification(bool enable);

    // Blocking. Single attempt, no retries.
    const HttpResponse& request(const HttpRequest& req);
    // Aborts an in-flight request(): the progress callback returns non-zero and the
    // response status becomes 20. Thread-safe.
    void cancel();

    // url + "?" + k=v joined by "&" (map order), no escaping. Exposed for tests.
    // Percent-encodes everything except RFC 3986 unreserved characters.
    static std::string urlEncode(const std::string& value);
    static std::string buildUrl(const std::string& url, const std::map<std::string, std::string>& query);
    // Authority part of a URL ("https://a.b:443/x" -> "a.b:443").
    static std::string hostOf(const std::string& url);
    // CURLcode -> pseudo status.
    static int statusFromCurlCode(int curlCode);

    struct Session;

private:
    std::shared_ptr<Session> openSession(const std::string& key);
    void closeSession(const std::shared_ptr<Session>& session);
    void configure(Session& session);

    unsigned timeout_ = 0;
    std::string userAgent_;
    const bool keepAlive_;
    std::mutex mutex_;
    std::shared_ptr<Session> session_;  // the in-flight session, for cancel()
    HttpResponse response_;
};

}  // namespace tcng::util

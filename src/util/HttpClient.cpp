#include "util/HttpClient.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include <curl/curl.h>

#include "util/Log.hpp"

namespace tcng::util {

// Session states; onProgressNotify aborts the transfer while the state is Canceling.
struct HttpClient::Session {
    enum State : int { Running = 0, Idle = 1, Canceling = 3, Destroyed = 4 };

    std::string key;
    CURL* curl = nullptr;
    curl_slist* headers = nullptr;
    std::atomic<int> state{Running};

    ~Session() {
        if (headers)
            curl_slist_free_all(headers);
        if (curl)
            curl_easy_cleanup(curl);
    }
};

namespace {

std::mutex g_caMutex;
std::string g_caPath;

std::mutex g_poolMutex;
std::vector<std::shared_ptr<HttpClient::Session>> g_keepaliveServers;

int onProgressNotify(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* session = static_cast<HttpClient::Session*>(clientp);
    return session->state.load() == HttpClient::Session::Canceling ? 1 : 0;
}

// onReceiveHeader: keep lines that contain ':'.
std::size_t onReceiveHeader(char* data, std::size_t size, std::size_t nmemb, void* userp) {
    auto* resp = static_cast<HttpResponse*>(userp);
    std::string line(data, size * nmemb);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line.find(':') != std::string::npos)
        resp->headers.push_back(std::move(line));
    return size * nmemb;
}

std::size_t onReceiveBody(char* data, std::size_t size, std::size_t nmemb, void* userp) {
    auto* resp = static_cast<HttpResponse*>(userp);
    resp->body.insert(resp->body.end(), data, data + size * nmemb);
    return size * nmemb;
}

}  // namespace

void HttpClient::globalInit(const std::string& caPath) {
    curl_global_init(CURL_GLOBAL_ALL);
    std::lock_guard lock(g_caMutex);
    if (!caPath.empty())
        g_caPath = caPath;
    log::get()->info("certification path: {}", g_caPath.empty() ? "<system default>" : g_caPath);
}

void HttpClient::globalDeinit() {}

void HttpClient::globalCloseAllKeepalive() {
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(g_poolMutex);
        sessions.swap(g_keepaliveServers);
    }
    for (auto& s : sessions)
        s->state = Session::Destroyed;
}

std::string HttpClient::caPath() {
    std::lock_guard lock(g_caMutex);
    return g_caPath;
}

HttpClient::HttpClient(bool keepAlive) : keepAlive_(keepAlive) {}

HttpClient::~HttpClient() = default;

void HttpClient::setTimeout(unsigned seconds) {
    timeout_ = seconds;
}

void HttpClient::setUserAgent(const std::string& userAgent) {
    userAgent_ = userAgent;
}

void HttpClient::enableServerVerification(bool enable) {
    if (!enable)
        log::get()->warn("HttpClient: server verification cannot be disabled; ignoring");
}

std::string HttpClient::urlEncode(const std::string& value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

std::string HttpClient::buildUrl(const std::string& url, const std::map<std::string, std::string>& query) {
    if (query.empty())
        return url;
    std::string out = url + "?";
    bool first = true;
    for (const auto& [k, v] : query) {
        if (!first)
            out += "&";
        first = false;
        out += urlEncode(k) + "=" + urlEncode(v);
    }
    return out;
}

std::string HttpClient::hostOf(const std::string& url) {
    auto begin = url.find("://");
    begin = begin == std::string::npos ? 0 : begin + 3;
    auto end = url.find('/', begin);
    return url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

int HttpClient::statusFromCurlCode(int code) {
    switch (code) {
    case CURLE_OK:
        return 200;
    case CURLE_OPERATION_TIMEDOUT:
        return 11;
    case 35: case 53: case 54: case 58: case 59: case 64: case 66:
    case 77: case 80: case 82: case 83: case 90: case 91:
        return 10;
    case CURLE_ABORTED_BY_CALLBACK:
        return 20;
    default:
        return 99;
    }
}

// Handle options. Re-applied after curl_easy_reset() when a pooled handle is
// reused, so no option of a previous request (e.g. POSTFIELDS) survives.
void HttpClient::configure(Session& session) {
    CURL* curl = session.curl;
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &onProgressNotify);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &session);
    const std::string ca = caPath();
    if (!ca.empty())
        curl_easy_setopt(curl, CURLOPT_CAPATH, ca.c_str());  // copied by libcurl
#ifdef _WIN32
    else
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_));
    if (!userAgent_.empty())
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent_.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
}

std::shared_ptr<HttpClient::Session> HttpClient::openSession(const std::string& key) {
    if (keepAlive_) {
        std::lock_guard lock(g_poolMutex);
        for (auto it = g_keepaliveServers.begin(); it != g_keepaliveServers.end(); ++it) {
            if ((*it)->key == key && (*it)->state == Session::Idle) {
                auto s = *it;
                g_keepaliveServers.erase(it);
                log::get()->trace("{}: Found keep alive session.", key);
                curl_easy_reset(s->curl);
                s->state = Session::Running;
                configure(*s);
                return s;
            }
        }
    }
    auto s = std::make_shared<Session>();
    s->key = key;
    s->curl = curl_easy_init();
    if (!s->curl) {
        log::get()->error("HttpClient curl_easy_init failed.");
        return nullptr;
    }
    log::get()->trace("{}: Create new session.", key);
    configure(*s);
    return s;
}

void HttpClient::closeSession(const std::shared_ptr<Session>& session) {
    if (session->headers) {
        curl_slist_free_all(session->headers);
        session->headers = nullptr;
    }
    if (keepAlive_ && session->state == Session::Running) {
        session->state = Session::Idle;
        std::lock_guard lock(g_poolMutex);
        g_keepaliveServers.push_back(session);
    } else {
        session->state = Session::Destroyed;
    }
}

const HttpResponse& HttpClient::request(const HttpRequest& req) {
    response_ = HttpResponse{};
    auto session = openSession(req.host.empty() ? hostOf(req.url) : req.host);
    if (!session) {
        response_.status = 1;
        return response_;
    }
    {
        std::lock_guard lock(mutex_);
        session_ = session;
    }
    CURL* curl = session->curl;

    // URL, headers, body
    const std::string url = buildUrl(req.url, req.query);
    log::get()->debug("curl set url:\"{}\"", url);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    for (const auto& h : req.headers)
        session->headers = curl_slist_append(session->headers, h.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, session->headers);
    if (!req.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
    }

    // Method
    const char* custom = nullptr;
    switch (req.method) {
    case HttpMethod::Get:
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        custom = "GET";
        break;
    case HttpMethod::Post:
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        custom = "POST";
        break;
    case HttpMethod::Put:
        custom = "PUT";
        break;
    case HttpMethod::Delete:
        custom = "DELETE";
        break;
    default: {
        response_.status = 400;
        const std::string msg = "Invalid HTTP Method called. method=" + std::to_string(static_cast<int>(req.method));
        response_.body.assign(msg.begin(), msg.end());
        closeSession(session);
        std::lock_guard lock(mutex_);
        session_.reset();
        return response_;
    }
    }
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, custom);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &onReceiveBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &onReceiveHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 120L);

    char errbuf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, nullptr);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        response_.status = static_cast<int>(code);
    } else {
        response_.status = statusFromCurlCode(rc);
        if (rc != CURLE_ABORTED_BY_CALLBACK)
            log::get()->error("curl_easy_perform failed. reason:{} further detail:{}", curl_easy_strerror(rc), errbuf);
        session->state = Session::Destroyed;  // don't pool a handle in an unknown state
    }
    closeSession(session);
    std::lock_guard lock(mutex_);
    session_.reset();
    return response_;
}

void HttpClient::cancel() {
    std::lock_guard lock(mutex_);
    if (session_) {
        int expected = Session::Running;
        if (session_->state.compare_exchange_strong(expected, Session::Canceling))
            log::get()->debug("{}: Set state CANCELING.", session_->key);
    }
}

}  // namespace tcng::util

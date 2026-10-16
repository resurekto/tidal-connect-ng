#pragma once
// HTTP request and response types.

#include <map>
#include <string>
#include <vector>

namespace tcng::util {

// HttpRequest::Method: 0 GET, 1 POST, 2 PUT, 3 DELETE.
enum class HttpMethod : int { Get = 0, Post = 1, Put = 2, Delete = 3 };

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    std::string url;
    // Keep-alive pool key: the authority between "://" and the next "/" (the generic
    // engine fills it explicitly). Empty: derived from url.
    std::string host;
    // Emitted by HttpClient as "?k=v&k=v" in key order, percent-encoded.
    std::map<std::string, std::string> query;
    std::vector<std::string> headers;  // verbatim "Name: value" lines
    std::string body;
};

// Pseudo statuses besides real HTTP codes:
//   1 no session / curl handle, 10 TLS failure, 11 timeout, 20 cancelled, 99 other curl
//   error, 400 invalid method.
struct HttpResponse {
    int status = 0;
    std::vector<std::string> headers;  // "Name: value" lines as received (CR/LF stripped)
    std::vector<char> body;

    std::string bodyString() const { return {body.begin(), body.end()}; }
};

}  // namespace tcng::util

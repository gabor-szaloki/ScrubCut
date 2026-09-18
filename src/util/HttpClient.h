#pragma once

#include <string>

// Blocking HTTPS GET on the OS stack (WinHTTP / NSURLSession) for small,
// infrequent requests from a worker thread. True with the body on 2xx,
// otherwise false with a short reason in outError. Follows redirects.
bool HttpGet(const std::string& url, std::string& outBody, std::string& outError,
             int timeoutMs = 15000);

#include "HTTP.h"
#include <sstream>

namespace ts {

void HttpHeader::parse(const std::string& raw) {
    std::istringstream stream(raw);
    std::string line;
    
    // Parse first line (request/status line)
    if (std::getline(stream, line)) {
        std::istringstream firstLine(line);
        firstLine >> method >> url >> version;
    }
    
    // Parse headers
    while (std::getline(stream, line) && !line.empty()) {
        size_t colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);
            
            // Trim whitespace
            while (!value.empty() && std::isspace(value[0])) {
                value = value.substr(1);
            }
            
            fields[key] = value;
        }
    }
}

std::string HttpHeader::toString() const {
    std::ostringstream stream;
    stream << method << " " << url << " " << version << "\r\n";
    
    for (const auto& [key, value] : fields) {
        stream << key << ": " << value << "\r\n";
    }
    
    stream << "\r\n";
    return stream.str();
}

HttpRequest HttpRequest::parse(const char* data, size_t length) {
    HttpRequest request;
    std::string raw(data, length);
    
    // Find the end of headers
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        headerEnd = raw.length();
    }
    
    // Parse headers
    request.header.parse(raw.substr(0, headerEnd));
    
    // Get body
    if (headerEnd + 4 < raw.length()) {
        const char* bodyStart = data + headerEnd + 4;
        size_t bodyLength = length - (headerEnd + 4);
        request.body.assign(bodyStart, bodyStart + bodyLength);
    }
    
    return request;
}

std::string HttpRequest::toString() const {
    std::string result = header.toString();
    if (!body.empty()) {
        result.append(body.begin(), body.end());
    }
    return result;
}

HttpResponse HttpResponse::parse(const char* data, size_t length) {
    HttpResponse response;
    std::string raw(data, length);
    
    // Find the end of headers
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        headerEnd = raw.length();
    }
    
    // Parse headers
    response.header.parse(raw.substr(0, headerEnd));
    
    // Get body
    if (headerEnd + 4 < raw.length()) {
        const char* bodyStart = data + headerEnd + 4;
        size_t bodyLength = length - (headerEnd + 4);
        response.body.assign(bodyStart, bodyStart + bodyLength);
    }
    
    return response;
}

std::string HttpResponse::toString() const {
    std::string result = header.toString();
    if (!body.empty()) {
        result.append(body.begin(), body.end());
    }
    return result;
}

bool HttpCache::lookup(const std::string& key, CacheEntry& entry) {
    auto it = entries.find(key);
    if (it != entries.end()) {
        entry = it->second;
        return true;
    }
    return false;
}

void HttpCache::update(const std::string& key, const CacheEntry& entry) {
    entries[key] = entry;
}

void HttpCache::remove(const std::string& key) {
    entries.erase(key);
}

} // namespace ts

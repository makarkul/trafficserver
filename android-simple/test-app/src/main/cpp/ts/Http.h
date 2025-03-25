#pragma once

#include <string>
#include <map>
#include <vector>

namespace ts {

class HttpHeader {
public:
    std::string method;
    std::string url;
    std::string version;
    std::map<std::string, std::string> fields;
    
    void parse(const std::string& raw);
    std::string toString() const;
};

class HttpRequest {
public:
    HttpHeader header;
    std::vector<char> body;
    
    static HttpRequest parse(const char* data, size_t length);
    std::string toString() const;
};

class HttpResponse {
public:
    HttpHeader header;
    std::vector<char> body;
    
    static HttpResponse parse(const char* data, size_t length);
    std::string toString() const;
};

class HttpCache {
public:
    struct CacheEntry {
        HttpResponse response;
        long timestamp;
        std::string etag;
    };
    
    bool lookup(const std::string& key, CacheEntry& entry);
    void update(const std::string& key, const CacheEntry& entry);
    void remove(const std::string& key);
    
private:
    std::map<std::string, CacheEntry> entries;
};

class HttpRemap {
public:
    struct RemapRule {
        std::string fromHost;
        std::string fromPath;
        std::string toHost;
        std::string toPath;
    };
    
    void addRule(const RemapRule& rule);
    bool remap(std::string& host, std::string& path);
    
private:
    std::vector<RemapRule> rules;
};

} // namespace ts

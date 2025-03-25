#pragma once

#include "ts/I_EventSystem.h"
#include "ts/I_Net.h"
#include "ts/HTTP.h"
#include "ts/HttpConfig.h"

class TSBridge {
public:
    static void initialize();
    static void processRequest(const char* request, size_t length);
    static void processResponse(const char* response, size_t length);
    
    // TrafficServer event system integration
    static EventProcessor* eventProcessor;
    static NetProcessor* netProcessor;
    
    // HTTP processing
    static ts::HttpRequest* createRequest();
    static ts::HttpResponse* createResponse();
    static void parseRequest(ts::HttpRequest* req, const char* buf, size_t len);
    static void parseResponse(ts::HttpResponse* resp, const char* buf, size_t len);
    
    // Cache operations
    static bool lookupCache(const char* url, char* buffer, size_t* len);
    static void updateCache(const char* url, const char* data, size_t len);
    
 private:
    static bool initialized;
    static void initializeEventSystem();
    static void initializeNetProcessor();
    static void initializeHTTP();
};

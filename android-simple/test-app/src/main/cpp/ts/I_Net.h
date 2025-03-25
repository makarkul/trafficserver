#pragma once

class NetProcessor {
public:
    NetProcessor() = default;
    virtual ~NetProcessor() = default;
    
    // Add basic networking functionality as needed
    void start() {}
    void stop() {}
};

// Global net processor instance
extern NetProcessor* netProcessor;

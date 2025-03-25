#pragma once

class EventProcessor {
public:
    EventProcessor() = default;
    virtual ~EventProcessor() = default;
    
    // Add basic event system functionality as needed
    void start() {}
    void stop() {}
};

// Global event processor instance
extern EventProcessor* eventProcessor;

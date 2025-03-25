#pragma once

namespace ts {

struct HttpConfig {
    static constexpr int DEFAULT_PORT = 8080;
    static constexpr int CACHE_TTL = 60; // seconds
    static constexpr bool ENABLE_CACHING = true;
    static constexpr bool ENABLE_REMAPPING = true;
};

} // namespace ts

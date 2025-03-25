# TrafficServer Android Simple

A lightweight implementation of Apache TrafficServer features for Android, demonstrating core proxy server functionality, URL remapping, and caching.

## Features

### 1. Proxy Server
- HTTP and HTTPS support
- Custom headers for tracking
- Foreground service for persistent operation
- Port 8888 by default

### 2. URL Remapping
- Configuration via `remap.config`
- Support for `map` directives
- Dynamic rule loading
- Example:
  ```
  map http://example.com/ http://info.cern.ch/
  map https://example.com/ https://api.github.com/zen
  ```

### 3. Caching
- In-memory cache with TTL (60s)
- Cache hit/miss indicators
- Cache age tracking
- Content-Type and ETag preservation
- Cache control headers:
  - X-Cache: HIT/MISS
  - X-Cache-Age: seconds
  - Via: TrafficServer-Android

## Building

1. Clone the repository
2. Open in Android Studio
3. Build and run

## Testing

The app includes a test interface with:
- URL dropdown for testing different endpoints
- Direct vs. Proxy request comparison
- Response headers and content display
- Cache status indicators

## Requirements
- Android SDK 21+
- CMake for native code
- Android NDK

# Cache Testing Guide

The TrafficServer proxy supports pre-populating the cache for testing purposes. This guide explains how to manually create and test cached content.

## Methods for Cache Population

There are two ways to populate the cache:

### Cache File Location
Cache files are stored in the app's private directory on the Android device:
```
/data/user/0/org.apache.trafficserver.test/app_config/cache/
```

### 1. Using the Cache Population Script (From Host Machine)
1. Use `create_cache_entry.py` to generate a cache file:
```bash
python3 create_cache_entry.py
```

The script will:
- Create a cache file with test content
- Use the correct binary format expected by the proxy
- Print the hash/filename for the cache entry

### Pushing Cache Files to Device
To push a cache file to the device:
```bash
# First push to accessible temp location
adb push cache_entry /data/local/tmp/

# Then copy to app's cache directory using run-as
adb shell "run-as org.apache.trafficserver.test cp /data/local/tmp/cache_entry /data/user/0/org.apache.trafficserver.test/app_config/cache/<hash>"
```

Replace `<hash>` with the filename output by the cache creation script.

### 2. Using the Cache Population API (From Emulator)

The proxy provides a simple HTTP API to populate the cache:

```bash
# Format:
# http://cache.local/populate?url=<target_url>&content=<content_to_cache>

# Example using netcat (from emulator):
echo -e 'GET http://cache.local/populate?url=example.com&content=hello HTTP/1.1\r\nHost: localhost\r\n\r\n' | nc 127.0.0.1 8888

# To URL-encode more complex content:
echo -e 'GET http://cache.local/populate?url=example.com&content=%3Chtml%3E%3Cbody%3EHello%3C%2Fbody%3E%3C%2Fhtml%3E HTTP/1.1\r\nHost: localhost\r\n\r\n' | nc 127.0.0.1 8888
```

### Testing Cached Content
1. Start the test app
2. Click "Start Proxy" to start the proxy service
3. Select the URL from the dropdown that matches your cached content
4. Click "via proxy" to make the request
5. The proxy should serve your cached content instead of making a network request

### Cache File Format
Cache files use the following binary format:
```
[timestamp (8 bytes)]
[content-type length (8 bytes)][content-type string]
[etag length (8 bytes)][etag string]
[content length (8 bytes)][content string]
```

### Cache Key Format
- Cache keys are in the format: `hostname/path/`
- Example: For URL `http://example.com`, the cache key is `example.com/`
- The filename is the std::hash of this key

### Debugging Cache Access
The proxy logs cache operations to logcat. View them with:
```bash
adb logcat -s ProxyService
```

Look for lines containing:
- "Cache key:" - Shows the key being looked up
- "Looking for cache file at:" - Shows the full path being checked
- "Served from cache:" - Confirms a cache hit

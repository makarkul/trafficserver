# TrafficServer Android Simple

A lightweight implementation of Apache TrafficServer features for Android, demonstrating core proxy server functionality, URL remapping, and caching.

## Comparison with Apache TrafficServer

### Architecture

| Feature | Apache TrafficServer | Android Simple TrafficServer |
|---------|---------------------|----------------------------|
| Platform | Linux/Unix systems | Android (Java/Native) |
| Architecture | Multi-threaded event-driven | Single-process Android service |
| Memory Model | Shared memory for cache | In-memory cache per process |
| Configuration | Complex config files system | Programmatic configuration |
| Scale | Enterprise-grade, high-performance | Single device, lightweight |

### Features

| Feature | Apache TrafficServer | Android Simple TrafficServer |
|---------|---------------------|----------------------------|
| HTTP/HTTPS | Full support with advanced features | Basic support |
| Caching | Disk-based with RAM cache | In-memory only (60s TTL) |
| URL Remapping | Complex rules with regex support | Basic map directives |
| Plugins | Extensive plugin system | No plugin support |
| Protocols | HTTP/1.x, HTTP/2, TLS | HTTP/1.1 only |
| Monitoring | Traffic Top, Stats API | Basic logging and UI stats |

### Use Cases

**Apache TrafficServer**
- Enterprise CDN deployments
- Large-scale reverse proxy
- Complex caching hierarchies
- High-throughput environments
- Multi-datacenter setups

**Android Simple TrafficServer**
- Mobile app proxy needs
- Development and testing
- Learning/Educational purposes
- Single-device caching
- Basic URL remapping

### Performance

| Aspect | Apache TrafficServer | Android Simple TrafficServer |
|--------|---------------------|----------------------------|
| Connections | 10000s concurrent | 10s concurrent |
| Cache Size | Multiple TB | Limited by device memory |
| Throughput | Gbps range | Limited by mobile network |
| Latency | Sub-millisecond | Milliseconds range |

### Key Differences

1. **Simplicity vs Functionality**
   - Apache TS: Full-featured, complex configuration
   - Android Simple: Minimalist, programmatic setup

2. **Resource Usage**
   - Apache TS: Optimized for server hardware
   - Android Simple: Optimized for mobile devices

3. **Development Focus**
   - Apache TS: Enterprise features, scalability
   - Android Simple: Android integration, simplicity

4. **Target Environment**
   - Apache TS: Data centers, edge servers
   - Android Simple: Individual Android devices

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

## Prerequisites

1. Android Development Environment:
   - Android SDK (minimum API 21)
   - Android NDK
   - CMake
   - Java Development Kit (JDK) 11 or higher

2. Environment Variables:
   ```bash
   export ANDROID_SDK_ROOT=$HOME/Library/Android/sdk
   export ANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/latest
   export PATH=$PATH:$ANDROID_SDK_ROOT/platform-tools:$ANDROID_SDK_ROOT/tools/bin
   ```

## Step-by-Step Setup

1. Clone the repository:
   ```bash
   git clone https://github.com/makarkul/trafficserver.git
   cd trafficserver
   git checkout feature/android-simple-trafficserver
   ```

2. Configure SDK path:
   ```bash
   cd android-simple
   echo "sdk.dir=$HOME/Library/Android/sdk" > local.properties
   ```

3. Build the project:
   ```bash
   ./gradlew build
   ```

4. Start an Android emulator:
   ```bash
   $ANDROID_SDK_ROOT/emulator/emulator -list-avds  # List available emulators
   $ANDROID_SDK_ROOT/emulator/emulator -avd <emulator_name>  # Start emulator
   ```

5. Install and run the app:
   ```bash
   $ANDROID_SDK_ROOT/platform-tools/adb install -r test-app/build/outputs/apk/debug/test-app-debug.apk
   $ANDROID_SDK_ROOT/platform-tools/adb shell am start -n org.apache.trafficserver.test/.MainActivity
   ```

## Testing the Proxy

1. Open the app on the emulator
2. Enter a URL (e.g., http://example.com)
3. Check "Use Proxy" checkbox
4. Tap "Make Request"

## Verifying Proxy Operation

Monitor logs using:
```bash
# Clear existing logs
adb logcat -c

# Monitor proxy-related logs
adb logcat | grep -i "proxyservice\|proxytest\|tsbridge\|mainactivity"
```

Expected log patterns:
1. Proxy service start:
   ```
   ProxyService: Proxy server listening on port 8888
   ```
2. Request handling:
   ```
   ProxyService: Received request: GET http://example.com/ HTTP/1.1
   ProxyService: Remapping request: example.com/ -> info.cern.ch/
   ```
3. Cache operation:
   ```
   ProxyService: Cache miss for info.cern.ch/
   ProxyService: Cached response for info.cern.ch/ (size: XXX bytes)
   ```

## Troubleshooting

### 1. Proxy Service Not Starting
If the proxy service doesn't start on the first attempt:
- Check logcat for native library loading issues
- Verify the app has network permissions
- Try restarting the app
- Check if port 8888 is already in use

### 2. Environment Setup Issues
If build fails due to missing SDK:
1. Verify environment variables:
   ```bash
   echo $ANDROID_SDK_ROOT
   echo $ANDROID_NDK_ROOT
   ```
2. Check local.properties contains correct SDK path
3. Ensure all required SDK components are installed:
   ```bash
   sdkmanager --list | grep installed
   ```

### 3. Build Warnings
Common warnings and solutions:
- Case sensitivity in include paths (HTTP.h vs Http.h)
  - Fix: Update include statements to match exact file names
- Java 8 deprecation warnings
  - Fix: Update Java version in build.gradle
- Gradle deprecation warnings
  - Fix: Update Gradle configuration as suggested in warnings

## Logs and Debugging

### Monitor Proxy Service
```bash
# Check service status
adb shell dumpsys activity services org.apache.trafficserver.test

# Monitor real-time logs with timestamps
adb logcat -v time | grep -i "proxyservice\|proxytest\|tsbridge\|mainactivity\|error\|exception"
```

### Interpreting Logs

#### 1. Service Startup Success
Look for these logs in sequence:
```
# Service creation
I ActivityManager: Background started FGS: [...org.apache.trafficserver.test/.TrafficServerProxyService]

# Native library loading
D nativeloader: Load libtrafficserver_test.so [...]: ok

# Proxy initialization
I ProxyService: Added remap rule: example.com/ -> info.cern.ch/
I ProxyService: Proxy server listening on port 8888
```

#### 2. Successful Request Flow
A properly working request should show:
```
# Client setup
D ProxyTest: Creating client with proxy...
D ProxyTest: Created client with proxy: HTTP @ /127.0.0.1:8888

# Request handling
I ProxyService: Received request: GET http://example.com/ HTTP/1.1
I ProxyService: Target URL: http://example.com/
I ProxyService: Remapping request: example.com/ -> info.cern.ch/

# Cache operation (first request)
I ProxyService: Cache miss for info.cern.ch/
I ProxyService: Cached response for info.cern.ch/ (size: XXX bytes)

# Cache operation (subsequent request)
I ProxyService: Cache hit for info.cern.ch/
```

#### 3. Common Error Patterns
Watch for these issues:
```
# Service failed to start
E ProxyService: Failed to start proxy server on port 8888

# Native library issues
E nativeloader: Failed to load libtrafficserver_test.so

# Network errors
E ProxyService: Failed to connect to remote host
E ProxyService: Connection timed out
```

### Verifying Successful Operation
To confirm the proxy is working correctly:

1. Check Service Status:
   ```bash
   adb shell dumpsys activity services org.apache.trafficserver.test | grep -A2 "ServiceRecord"
   ```
   Should show: `isForeground=true` and `startRequested=true`

2. Verify Port Listening:
   ```bash
   adb shell netstat | grep 8888
   ```
   Should show the proxy listening on port 8888

3. Test Cache Behavior:
   - Make same request twice
   - First request: Look for `Cache miss` followed by `Cached response`
   - Second request: Should show `Cache hit`

### View Cache Status
The app UI shows cache status in response headers:
- X-Cache: HIT/MISS (Confirms if response was served from cache)
- X-Cache-Age: Time since caching (Should be < 60s for valid cache)
- Via: TrafficServer-Android (Confirms proxy processed the request)

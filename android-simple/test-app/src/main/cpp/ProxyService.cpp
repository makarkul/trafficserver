#include "ProxyService.h"
#include <jni.h>
#include <android/log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <thread>
#include <atomic>
#include <netdb.h>
#include <arpa/inet.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <chrono>

#define LOG_TAG "ProxyService"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct RemapRule {
    std::string fromHost;
    std::string fromPath;
    std::string toHost;
    std::string toPath;
};

static std::atomic<bool> proxyRunning{false};
static int serverSocket = -1;
static std::thread proxyThread;
static std::vector<RemapRule> remapRules;

struct CacheEntry {
    std::string content;
    std::string contentType;
    std::chrono::system_clock::time_point timestamp;
    std::string etag;
};

static std::map<std::string, CacheEntry> cache;

void* proxyLoop(void*) {
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        LOGE("Failed to create socket");
        return nullptr;
    }

    // Enable address reuse
    int opt = 1;
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOGE("Failed to set socket options");
        close(serverSocket);
        return nullptr;
    }

    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(8888); // Port for proxy server

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        LOGE("Failed to bind socket");
        close(serverSocket);
        return nullptr;
    }

    if (listen(serverSocket, 5) < 0) {
        LOGE("Failed to listen on socket");
        close(serverSocket);
        return nullptr;
    }

    LOGI("Proxy server listening on port 8888");

    while (proxyRunning) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);
        
        if (clientSocket < 0) {
            if (proxyRunning) {
                LOGE("Failed to accept connection");
            }
            continue;
        }

        // Handle client connection in a new thread
        std::thread([clientSocket]() {
            char buffer[8192];
            ssize_t n = recv(clientSocket, buffer, sizeof(buffer)-1, 0);
            if (n > 0) {
                buffer[n] = '\0';
                LOGI("Received request: %s", buffer);

                // Parse the request line to get the target URL
                std::string request(buffer);
                size_t firstSpace = request.find(' ');
                size_t secondSpace = request.find(' ', firstSpace + 1);
                if (firstSpace != std::string::npos && secondSpace != std::string::npos) {
                    std::string url = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
                    LOGI("Target URL: %s", url.c_str());

                    // Create a socket to connect to the target server
                    struct addrinfo hints = {}, *res;
                    hints.ai_family = AF_UNSPEC;
                    hints.ai_socktype = SOCK_STREAM;

                    // Parse URL to get host and path
                    std::string host, path, port = "80";
                    bool isConnect = request.substr(0, firstSpace) == "CONNECT";

                    if (isConnect) {
                        // For CONNECT, URL is in the form host:port
                        size_t colonPos = url.find(':');
                        if (colonPos != std::string::npos) {
                            host = url.substr(0, colonPos);
                            port = url.substr(colonPos + 1);
                        } else {
                            host = url;
                            port = "443";
                        }
                        path = "/";
                    } else if (url.substr(0, 7) == "http://") {
                        size_t hostStart = 7;
                        size_t hostEnd = url.find('/', hostStart);
                        if (hostEnd == std::string::npos) {
                            host = url.substr(hostStart);
                            path = "/";
                        } else {
                            host = url.substr(hostStart, hostEnd - hostStart);
                            path = url.substr(hostEnd);
                        }

                        // Check for remap rules
                        bool remapped = false;
                        for (const auto& rule : remapRules) {
                            if (host == rule.fromHost && (path == rule.fromPath || rule.fromPath == "/")) {
                                LOGI("Remapping request: %s%s -> %s%s",
                                     host.c_str(), path.c_str(),
                                     rule.toHost.c_str(), rule.toPath.c_str());
                                host = rule.toHost;
                                path = rule.toPath;
                                remapped = true;
                                break;
                            }
                        }
                        if (!remapped) {
                            LOGI("No remap rule found for: %s%s", host.c_str(), path.c_str());
                        }
                    } else {
                        LOGE("Only HTTP and HTTPS (via CONNECT) are supported");
                        close(clientSocket);
                        return;
                    }

                    LOGI("Host: %s, Path: %s", host.c_str(), path.c_str());

                    // Connect to target server
                    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0) {
                        int serverSock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                        if (serverSock >= 0 && connect(serverSock, res->ai_addr, res->ai_addrlen) == 0) {
                            if (isConnect) {
                                // For CONNECT, send 200 OK to client
                                std::string response = "HTTP/1.1 200 Connection established\r\n"
                                                     "Proxy-Agent: TrafficServer\r\n"
                                                     "X-Proxy-Through: TrafficServer-Android\r\n"
                                                     "\r\n";
                                send(clientSocket, response.c_str(), response.length(), 0);

                                // Now tunnel data between client and server
                                fd_set readfds;
                                int maxfd = std::max(clientSocket, serverSock) + 1;

                                while (true) {
                                    FD_ZERO(&readfds);
                                    FD_SET(clientSocket, &readfds);
                                    FD_SET(serverSock, &readfds);

                                    if (select(maxfd, &readfds, nullptr, nullptr, nullptr) < 0) {
                                        break;
                                    }

                                    if (FD_ISSET(clientSocket, &readfds)) {
                                        n = recv(clientSocket, buffer, sizeof(buffer), 0);
                                        if (n <= 0) break;
                                        send(serverSock, buffer, n, 0);
                                    }

                                    if (FD_ISSET(serverSock, &readfds)) {
                                        n = recv(serverSock, buffer, sizeof(buffer), 0);
                                        if (n <= 0) break;
                                        send(clientSocket, buffer, n, 0);
                                    }
                                }
                            } else {
                                // For HTTP, forward modified request
                                std::string modifiedRequest = "GET " + path + " HTTP/1.1\r\n"
                                                            "Host: " + host + "\r\n"
                                                            "Connection: close\r\n"
                                                            "X-Proxy-Through: TrafficServer-Android\r\n"
                                                            "\r\n";
                                send(serverSock, modifiedRequest.c_str(), modifiedRequest.length(), 0);

                                // Check cache first
                                std::string cacheKey = host + path;
                                bool cacheHit = false;
                                
                                auto cacheIt = cache.find(cacheKey);
                                if (cacheIt != cache.end()) {
                                    auto now = std::chrono::system_clock::now();
                                    auto age = std::chrono::duration_cast<std::chrono::seconds>(
                                        now - cacheIt->second.timestamp).count();
                                    
                                    // Cache TTL of 60 seconds for testing
                                    if (age < 60) {
                                        cacheHit = true;
                                        LOGI("Cache hit for %s (age: %lld seconds)", cacheKey.c_str(), age);
                                        
                                        // Send cached response with cache headers
                                        std::string cachedResponse = "HTTP/1.1 200 OK\r\n";
                                        cachedResponse += "Content-Type: " + cacheIt->second.contentType + "\r\n";
                                        cachedResponse += "Content-Length: " + std::to_string(cacheIt->second.content.length()) + "\r\n";
                                        cachedResponse += "X-Cache: HIT\r\n";
                                        cachedResponse += "X-Cache-Age: " + std::to_string(age) + "\r\n";
                                        cachedResponse += "X-Proxy-Through: TrafficServer-Android\r\n";
                                        cachedResponse += "Via: 1.1 TrafficServer-Android\r\n";
                                        if (!cacheIt->second.etag.empty()) {
                                            cachedResponse += "ETag: " + cacheIt->second.etag + "\r\n";
                                        }
                                        cachedResponse += "\r\n";
                                        cachedResponse += cacheIt->second.content;
                                        
                                        send(clientSocket, cachedResponse.c_str(), cachedResponse.length(), 0);
                                    }
                                }
                                
                                if (!cacheHit) {
                                    LOGI("Cache miss for %s", cacheKey.c_str());
                                    
                                    // Read the response headers first
                                    std::string responseHeaders;
                                    std::string responseBody;
                                    bool headersDone = false;
                                    int headerBytes = 0;
                                    std::string contentType;
                                    std::string etag;
                                    
                                    while (!headersDone && (n = recv(serverSock, buffer, sizeof(buffer), 0)) > 0) {
                                        std::string chunk(buffer, n);
                                        responseHeaders += chunk;
                                        
                                        // Look for end of headers (\r\n\r\n)
                                        size_t pos = responseHeaders.find("\r\n\r\n");
                                        if (pos != std::string::npos) {
                                            headersDone = true;
                                            headerBytes = pos + 4;  // Include the \r\n\r\n
                                            // Parse content type
                                            std::istringstream headerStream(responseHeaders);
                                            std::string line;
                                            while (std::getline(headerStream, line)) {
                                                if (line.find("Content-Type: ") == 0) {
                                                    contentType = line.substr(14);
                                                } else if (line.find("ETag: ") == 0) {
                                                    etag = line.substr(6);
                                                }
                                            }
                                            
                                            // Insert our headers just before the \r\n\r\n
                                            std::string modifiedHeaders = responseHeaders.substr(0, pos);
                                            modifiedHeaders += "\r\nX-Cache: MISS";
                                            modifiedHeaders += "\r\nX-Proxy-Through: TrafficServer-Android";
                                            modifiedHeaders += "\r\nVia: 1.1 TrafficServer-Android";
                                            modifiedHeaders += "\r\n\r\n";
                                            
                                            // Send modified headers
                                            send(clientSocket, modifiedHeaders.c_str(), modifiedHeaders.length(), 0);
                                            
                                            // Start collecting response body
                                            if (n > headerBytes) {
                                                responseBody = std::string(buffer + headerBytes, n - headerBytes);
                                                send(clientSocket, buffer + headerBytes, n - headerBytes, 0);
                                            }
                                        }
                                    }
                                    
                                    // Collect and forward remaining response body
                                    while ((n = recv(serverSock, buffer, sizeof(buffer), 0)) > 0) {
                                        responseBody += std::string(buffer, n);
                                        send(clientSocket, buffer, n, 0);
                                    }
                                    
                                    // Cache the response if it's cacheable (for this demo, we cache everything)
                                    CacheEntry entry;
                                    entry.content = responseBody;
                                    entry.contentType = contentType;
                                    entry.timestamp = std::chrono::system_clock::now();
                                    entry.etag = etag;
                                    cache[cacheKey] = entry;
                                    
                                    LOGI("Cached response for %s (size: %zu bytes)", 
                                         cacheKey.c_str(), responseBody.length());
                                }
                            }

                            close(serverSock);
                        } else {
                            LOGE("Failed to connect to target server");
                        }
                        freeaddrinfo(res);
                    } else {
                        LOGE("Failed to resolve host");
                    }
                }
            }
            close(clientSocket);
        }).detach();
    }

    close(serverSocket);
    return nullptr;
}

extern "C" {

JNIEXPORT jint JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_startProxy(
        JNIEnv *env, jobject thiz, jstring configPath) {
    if (proxyRunning) {
        return 1; // Already running
    }

    // Clear existing rules
    remapRules.clear();

    // Read and parse remap.config
    const char* configPathStr = env->GetStringUTFChars(configPath, nullptr);
    if (configPathStr) {
        std::string configDir(configPathStr);
        std::string remapConfigPath = configDir + "/remap.config";
        std::ifstream remapFile(remapConfigPath);
        std::string line;

        while (std::getline(remapFile, line)) {
            // Skip comments and empty lines
            if (line.empty() || line[0] == '#' || line[0] == ' ') continue;

            std::istringstream iss(line);
            std::string cmd, from, to;
            if (iss >> cmd >> from >> to) {
                if (cmd == "map") {
                    RemapRule rule;
                    
                    // Parse from URL
                    size_t fromHostStart = from.find("//") + 2;
                    size_t fromHostEnd = from.find('/', fromHostStart);
                    rule.fromHost = from.substr(fromHostStart, fromHostEnd - fromHostStart);
                    rule.fromPath = fromHostEnd != std::string::npos ? from.substr(fromHostEnd) : "/";

                    // Parse to URL
                    size_t toHostStart = to.find("//") + 2;
                    size_t toHostEnd = to.find('/', toHostStart);
                    rule.toHost = to.substr(toHostStart, toHostEnd - toHostStart);
                    rule.toPath = toHostEnd != std::string::npos ? to.substr(toHostEnd) : "/";

                    LOGI("Added remap rule: %s%s -> %s%s",
                         rule.fromHost.c_str(), rule.fromPath.c_str(),
                         rule.toHost.c_str(), rule.toPath.c_str());

                    remapRules.push_back(rule);
                }
            }
        }

        env->ReleaseStringUTFChars(configPath, configPathStr);
    }

    proxyRunning = true;
    proxyThread = std::thread(proxyLoop, nullptr);
    return 0;
}

JNIEXPORT void JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_stopProxy(
        JNIEnv *env, jobject thiz) {
    if (!proxyRunning) {
        return;
    }

    proxyRunning = false;
    if (serverSocket >= 0) {
        close(serverSocket); // This will cause accept() to return with an error
    }
    if (proxyThread.joinable()) {
        proxyThread.join();
    }
}

JNIEXPORT jboolean JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_isProxyRunning(
        JNIEnv *env, jobject thiz) {
    return proxyRunning ? JNI_TRUE : JNI_FALSE;
}

}

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
#include <filesystem>
#include <fstream>

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

class DiskCache {
public:
    struct CacheEntry {
        std::string content;
        std::string contentType;
        std::chrono::system_clock::time_point timestamp;
        std::string etag;
    };

    DiskCache(const std::string& cacheDir) : cacheDir_(cacheDir) {
        std::filesystem::create_directories(cacheDir);
        LOGI("Initialized disk cache at: %s", cacheDir.c_str());
    }

    void put(const std::string& key, const CacheEntry& entry) {
        std::string path = getPath(key);
        std::ofstream file(path, std::ios::binary);
        if (file.is_open()) {
            // Write timestamp
            auto timestamp = std::chrono::system_clock::to_time_t(entry.timestamp);
            file.write(reinterpret_cast<const char*>(&timestamp), sizeof(timestamp));

            // Write content type length and content
            size_t len = entry.contentType.length();
            file.write(reinterpret_cast<const char*>(&len), sizeof(len));
            file.write(entry.contentType.c_str(), len);

            // Write etag length and content
            len = entry.etag.length();
            file.write(reinterpret_cast<const char*>(&len), sizeof(len));
            file.write(entry.etag.c_str(), len);

            // Write content length and content
            len = entry.content.length();
            file.write(reinterpret_cast<const char*>(&len), sizeof(len));
            file.write(entry.content.c_str(), len);

            LOGI("Cached %s (%zu bytes)", key.c_str(), entry.content.length());
        }
    }

    std::optional<CacheEntry> get(const std::string& key) {
        std::string path = getPath(key);
        LOGI("Looking for cache file at: %s", path.c_str());
        std::ifstream file(path, std::ios::binary);
        if (file.is_open()) {
            CacheEntry entry;

            // Read timestamp
            std::time_t timestamp;
            file.read(reinterpret_cast<char*>(&timestamp), sizeof(timestamp));
            entry.timestamp = std::chrono::system_clock::from_time_t(timestamp);

            // Read content type
            size_t len;
            file.read(reinterpret_cast<char*>(&len), sizeof(len));
            entry.contentType.resize(len);
            file.read(&entry.contentType[0], len);

            // Read etag
            file.read(reinterpret_cast<char*>(&len), sizeof(len));
            entry.etag.resize(len);
            file.read(&entry.etag[0], len);

            // Read content
            file.read(reinterpret_cast<char*>(&len), sizeof(len));
            entry.content.resize(len);
            file.read(&entry.content[0], len);

            return entry;
        }
        return std::nullopt;
    }

private:
    std::string getPath(const std::string& key) {
        // Create a filename-safe hash of the key
        std::hash<std::string> hasher;
        size_t hash = hasher(key);
        std::string filename = std::to_string(hash);
        LOGI("Cache key: %s, Hash: %zu, Filename: %s", key.c_str(), hash, filename.c_str());
        return cacheDir_ + "/" + filename;
    }
    std::string cacheDir_;
};

static std::unique_ptr<DiskCache> cache;

// Forward declaration
static void handleClientConnection(int socket);

void* proxyLoop(void*) {
    LOGI("ProxyLoop: Creating socket...");
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        LOGE("Failed to create socket");
        return nullptr;
    }

    // Enable address reuse
    int opt = 1;
    LOGI("ProxyLoop: Setting socket options...");
    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOGE("Failed to set socket options");
        close(serverSocket);
        return nullptr;
    }

    // Bind to port 8888
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(8888);

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        LOGE("Failed to bind server socket");
        close(serverSocket);
        return nullptr;
    }

    // Start listening
    if (listen(serverSocket, SOMAXCONN) < 0) {
        LOGE("Failed to listen on server socket");
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

        // Create a new thread for handling this client
        std::thread(handleClientConnection, clientSocket).detach();
    }

    close(serverSocket);
    return nullptr;
}

// Function to handle a client connection
static void handleClientConnection(int socket) {
    char buffer[8192];
    ssize_t n = recv(socket, buffer, sizeof(buffer)-1, 0);
    if (n > 0) {
        buffer[n] = '\0';
        LOGI("Received request: %s", buffer);

        // Parse the request line to get the target URL
        std::string request(buffer);
        size_t firstSpace = request.find(' ');
        size_t secondSpace = request.find(' ', firstSpace + 1);
        if (firstSpace != std::string::npos && secondSpace != std::string::npos) {
            std::string method = request.substr(0, firstSpace);
            std::string url = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
            LOGI("Method: %s, Target URL: %s", method.c_str(), url.c_str());

            // Check if this is a cache population request
            if (url.find("http://cache.local/populate?") == 0) {
                // Parse target_url and content from query string
                size_t urlPos = url.find("url=");
                size_t contentPos = url.find("&content=");
                if (urlPos != std::string::npos && contentPos != std::string::npos) {
                    std::string target_url = url.substr(urlPos + 4, contentPos - (urlPos + 4));
                    std::string content = url.substr(contentPos + 9);
                    
                    // Create cache entry
                    DiskCache::CacheEntry entry;
                    entry.content = content;
                    entry.contentType = "text/plain";
                    entry.timestamp = std::chrono::system_clock::now();
                    entry.etag = "w/" + std::to_string(std::chrono::system_clock::to_time_t(entry.timestamp));
                    
                    // Add / to target_url if needed
                    if (target_url.back() != '/') {
                        target_url += "/";
                    }
                    
                    // Store in cache
                    if (cache) {
                        cache->put(target_url, entry);
                        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nSuccess";
                        send(socket, response.c_str(), response.length(), 0);
                        LOGI("Added to cache: %s", target_url.c_str());
                        return;
                    }
                }
                std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 20\r\n\r\nInvalid cache request";
                send(socket, response.c_str(), response.length(), 0);
                return;
            }

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
                // Extract host and path from URL
                size_t hostStart = 7; // after "http://"
                size_t hostEnd = url.find('/', hostStart);
                if (hostEnd == std::string::npos) {
                    host = url.substr(hostStart);
                    path = "/";
                } else {
                    host = url.substr(hostStart, hostEnd - hostStart);
                    path = url.substr(hostEnd);
                }

                // Check for port in host
                size_t colonPos = host.find(':');
                if (colonPos != std::string::npos) {
                    port = host.substr(colonPos + 1);
                    host = host.substr(0, colonPos);
                }
            } else {
                // Invalid URL
                LOGE("Invalid URL format");
                close(socket);
                return;
            }

            LOGI("Connecting to %s:%s", host.c_str(), port.c_str());

            if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0) {
                int serverSock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                if (serverSock >= 0) {
                    if (connect(serverSock, res->ai_addr, res->ai_addrlen) == 0) {
                        if (isConnect) {
                            // For HTTPS, just tunnel the connection
                            const char* response = "HTTP/1.1 200 Connection established\r\n\r\n";
                            send(socket, response, strlen(response), 0);

                            fd_set readfds;
                            while (true) {
                                FD_ZERO(&readfds);
                                FD_SET(socket, &readfds);
                                FD_SET(serverSock, &readfds);

                                int maxfd = std::max(socket, serverSock) + 1;
                                if (select(maxfd, &readfds, nullptr, nullptr, nullptr) < 0) {
                                    break;
                                }

                                if (FD_ISSET(socket, &readfds)) {
                                    n = recv(socket, buffer, sizeof(buffer), 0);
                                    if (n <= 0) break;
                                    if (send(serverSock, buffer, n, 0) <= 0) break;
                                }

                                if (FD_ISSET(serverSock, &readfds)) {
                                    n = recv(serverSock, buffer, sizeof(buffer), 0);
                                    if (n <= 0) break;
                                    if (send(socket, buffer, n, 0) <= 0) break;
                                }
                            }
                        } else {
                            // For HTTP, we can cache the response
                            std::string cacheKey = host + path;
                            auto cachedEntry = cache->get(cacheKey);

                            if (cachedEntry) {
                                // Use cached response
                                std::string response = "HTTP/1.1 200 OK\r\n";
                                response += "Content-Type: " + cachedEntry->contentType + "\r\n";
                                response += "Content-Length: " + std::to_string(cachedEntry->content.length()) + "\r\n";
                                if (!cachedEntry->etag.empty()) {
                                    response += "ETag: " + cachedEntry->etag + "\r\n";
                                }
                                response += "\r\n";
                                response += cachedEntry->content;

                                send(socket, response.c_str(), response.length(), 0);
                                LOGI("Served from cache: %s", cacheKey.c_str());
                            } else {
                                // Forward the original request
                                send(serverSock, buffer, n, 0);

                                // Read and forward the response
                                std::string responseData;
                                std::string contentType;
                                std::string etag;
                                ssize_t contentLength = -1;
                                bool chunked = false;

                                // Read response headers
                                while ((n = recv(serverSock, buffer, sizeof(buffer), 0)) > 0) {
                                    responseData.append(buffer, n);
                                    size_t headerEnd = responseData.find("\r\n\r\n");
                                    if (headerEnd != std::string::npos) {
                                        // Forward headers to client
                                        send(socket, responseData.c_str(), responseData.length(), 0);

                                        // Parse headers
                                        std::istringstream headerStream(responseData.substr(0, headerEnd));
                                        std::string line;
                                        while (std::getline(headerStream, line) && line != "\r") {
                                            if (line.substr(0, 14) == "Content-Length:") {
                                                contentLength = std::stoll(line.substr(15));
                                            } else if (line.substr(0, 13) == "Content-Type: ") {
                                                contentType = line.substr(13);
                                            } else if (line.substr(0, 6) == "ETag: ") {
                                                etag = line.substr(6);
                                            }
                                        }
                                        break;
                                    }
                                }

                                // Read and forward response body
                                std::string responseBody;
                                while ((n = recv(serverSock, buffer, sizeof(buffer), 0)) > 0) {
                                    responseBody.append(buffer, n);
                                    send(socket, buffer, n, 0);
                                }

                                // Cache the response
                                DiskCache::CacheEntry entry;
                                entry.content = responseBody;
                                entry.contentType = contentType;
                                entry.timestamp = std::chrono::system_clock::now();
                                entry.etag = etag;
                                cache->put(cacheKey, entry);
                                LOGI("Cached response for %s (size: %zu bytes)", 
                                     cacheKey.c_str(), responseBody.length());
                            }
                        }
                    } else {
                        LOGE("Failed to connect to target server");
                    }
                    close(serverSock);
                }
                freeaddrinfo(res);
            } else {
                LOGE("Failed to resolve host");
            }
        }
    }
    close(socket);
}

extern "C" {

JNIEXPORT jint JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_startProxy(
        JNIEnv *env, jobject thiz, jstring configPath) {
    if (proxyRunning) {
        return 0;
    }

    proxyRunning = true;

    // Initialize cache
    const char* cacheDir = env->GetStringUTFChars(configPath, nullptr);
    if (cacheDir) {
        cache = std::make_unique<DiskCache>(std::string(cacheDir) + "/cache");
        env->ReleaseStringUTFChars(configPath, cacheDir);
    }

    // Start proxy thread
    proxyThread = std::thread(proxyLoop, nullptr);
    proxyThread.detach();
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
        close(serverSocket);
        serverSocket = -1;
    }
}

JNIEXPORT jboolean JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_isProxyRunning(
        JNIEnv *env, jobject thiz) {
    return proxyRunning;
}

}

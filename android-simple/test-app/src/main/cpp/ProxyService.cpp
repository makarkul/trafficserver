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
#include <algorithm>
#include <filesystem>
#include <fstream>

#define LOG_TAG "ProxyService"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

struct RemapRule {
  std::string fromHost;
  std::string fromPath;
  std::string toHost;
  std::string toPath;
};

static std::atomic<bool>      proxyRunning{false};
static int                    serverSocket = -1;
static std::thread            proxyThread;
static std::vector<RemapRule> remapRules;

class DiskCache {
public:
    struct CacheEntry {
        std::string content;
        std::string contentType;
        std::string contentEncoding;
        std::chrono::system_clock::time_point timestamp;
        std::string etag;
    };

    explicit DiskCache(const std::string& cacheDir) : cacheDir_(cacheDir) {
        std::filesystem::create_directories(cacheDir);
        LOGI("Initialized disk cache at: %s", cacheDir.c_str());
    }

    void put(const std::string& key, const CacheEntry& entry) {
        LOGI("DiskCache::put called with key: %s", key.c_str());
        std::string path = getPath(key);
        std::ofstream file(path, std::ios::binary);
        if (file.is_open()) {
            try {
                // Write timestamp
                auto timestamp = std::chrono::system_clock::to_time_t(entry.timestamp);
                file.write(reinterpret_cast<const char*>(&timestamp), sizeof(timestamp));

                // Write content type length and content
                size_t len = entry.contentType.length();
                file.write(reinterpret_cast<const char*>(&len), sizeof(len));
                file.write(entry.contentType.c_str(), len);

                // Write content encoding length and content
                len = entry.contentEncoding.length();
                file.write(reinterpret_cast<const char*>(&len), sizeof(len));
                file.write(entry.contentEncoding.c_str(), len);

                // Write etag length and content
                len = entry.etag.length();
                file.write(reinterpret_cast<const char*>(&len), sizeof(len));
                file.write(entry.etag.c_str(), len);

                // Write content length and content
                len = entry.content.length();
                file.write(reinterpret_cast<const char*>(&len), sizeof(len));
                file.write(entry.content.c_str(), len);

                LOGI("Cached %s (%zu bytes)", key.c_str(), entry.content.length());
            } catch (const std::exception& e) {
                LOGE("Exception while writing cache: %s", e.what());
            }
        } else {
            LOGE("Failed to open cache file for writing: %s", path.c_str());
        }
    }

    std::optional<CacheEntry> get(const std::string& key) {
        std::string path = getPath(key);
        LOGI("Reading cache file: %s", path.c_str());
        std::ifstream file(path, std::ios::binary);
        if (file.is_open()) {
            try {
                CacheEntry entry;

                // Read timestamp
                std::time_t timestamp;
                file.read(reinterpret_cast<char*>(&timestamp), sizeof(timestamp));
                if (file.fail()) {
                    LOGE("Failed to read timestamp");
                    return std::nullopt;
                }
                entry.timestamp = std::chrono::system_clock::from_time_t(timestamp);

                // Read content type
                size_t len;
                file.read(reinterpret_cast<char*>(&len), sizeof(len));
                if (file.fail() || len > 1000000) {
                    LOGE("Failed to read content type length or invalid length: %zu", len);
                    return std::nullopt;
                }
                entry.contentType.resize(len);
                file.read(&entry.contentType[0], len);
                if (file.fail()) {
                    LOGE("Failed to read content type");
                    return std::nullopt;
                }
                LOGI("Read content type: %s", entry.contentType.c_str());

                // Read content encoding
                file.read(reinterpret_cast<char*>(&len), sizeof(len));
                if (file.fail() || len > 1000000) {
                    LOGE("Failed to read content encoding length or invalid length: %zu", len);
                    return std::nullopt;
                }
                entry.contentEncoding.resize(len);
                file.read(&entry.contentEncoding[0], len);
                if (file.fail()) {
                    LOGE("Failed to read content encoding");
                    return std::nullopt;
                }
                LOGI("Read content encoding: %s", entry.contentEncoding.c_str());

                // Read etag
                file.read(reinterpret_cast<char*>(&len), sizeof(len));
                if (file.fail() || len > 1000000) {
                    LOGE("Failed to read etag length or invalid length: %zu", len);
                    return std::nullopt;
                }
                entry.etag.resize(len);
                file.read(&entry.etag[0], len);
                if (file.fail()) {
                    LOGE("Failed to read etag");
                    return std::nullopt;
                }
                LOGI("Read etag: %s", entry.etag.c_str());

                // Read content
                file.read(reinterpret_cast<char*>(&len), sizeof(len));
                if (file.fail() || len > 100000000) {
                    LOGE("Failed to read content length or invalid length: %zu", len);
                    return std::nullopt;
                }
                entry.content.resize(len);
                file.read(&entry.content[0], len);
                if (file.fail()) {
                    LOGE("Failed to read content");
                    return std::nullopt;
                }
                LOGI("Successfully read cache entry with %zu bytes of content", len);

                return entry;
            } catch (const std::exception& e) {
                LOGE("Exception while reading cache: %s", e.what());
                return std::nullopt;
            }
        } else {
            LOGE("Failed to open cache file: %s", path.c_str());
        }
        return std::nullopt;
    }

private:
    std::string cacheDir_;

    std::string getPath(const std::string& key) const {
        std::hash<std::string> hasher;
        size_t hash = hasher(key);
        std::string filename = std::to_string(hash);
        LOGI("Cache key: %s, Hash: %zu, Filename: %s", key.c_str(), hash, filename.c_str());
        return cacheDir_ + "/" + filename;
    }
};

static std::unique_ptr<DiskCache> cache;

// Forward declaration
static void handleClientConnection(int socket);

void *
proxyLoop(void *)
{
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
  serverAddr.sin_family      = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port        = htons(8888);

  if (bind(serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
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
    socklen_t          clientLen    = sizeof(clientAddr);
    int                clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddr, &clientLen);

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
static void
handleClientConnection(int socket)
{
  char    buffer[8192];
  ssize_t n = recv(socket, buffer, sizeof(buffer) - 1, 0);
  if (n > 0) {
    buffer[n] = '\0';
    LOGI("Received request: %s", buffer);

    // Parse the request line to get the target URL
    std::string request(buffer);

// Check if this is a cache population request
if (method == "POST" && url == "/cache/populate") {
// Find Content-Type header
std::string contentType;
size_t contentTypePos = request.find("Content-Type:");
if (contentTypePos != std::string::npos) {
size_t endOfLine = request.find("\r\n", contentTypePos);
if (endOfLine != std::string::npos) {
contentType = request.substr(contentTypePos + 13, endOfLine - (contentTypePos + 13));
// Trim whitespace
contentType.erase(0, contentType.find_first_not_of(" "));
contentType.erase(contentType.find_last_not_of(" ") + 1);
}
}

// Find Host header
std::string targetHost;
size_t hostPos = request.find("\r\nHost:");
if (hostPos != std::string::npos) {
size_t endOfLine = request.find("\r\n", hostPos + 7);
if (endOfLine != std::string::npos) {
targetHost = request.substr(hostPos + 7, endOfLine - (hostPos + 7));
// Trim whitespace
targetHost.erase(0, targetHost.find_first_not_of(" "));
targetHost.erase(targetHost.find_last_not_of(" ") + 1);
}
}

// Find Target-Path header
std::string targetPath;
size_t pathPos = request.find("\r\nTarget-Path:");
if (pathPos != std::string::npos) {
size_t endOfLine = request.find("\r\n", pathPos + 13);
if (endOfLine != std::string::npos) {
targetPath = request.substr(pathPos + 13, endOfLine - (pathPos + 13));
// Trim whitespace
targetPath.erase(0, targetPath.find_first_not_of(" "));
targetPath.erase(targetPath.find_last_not_of(" ") + 1);
}
}

// Find Content-Length header
ssize_t contentLength = -1;
size_t contentLengthPos = request.find("Content-Length:");
if (contentLengthPos != std::string::npos) {
size_t endOfLine = request.find("\r\n", contentLengthPos);
if (endOfLine != std::string::npos) {
std::string lengthStr = request.substr(contentLengthPos + 15, endOfLine - (contentLengthPos + 15));
// Trim whitespace
lengthStr.erase(0, lengthStr.find_first_not_of(" "));
lengthStr.erase(lengthStr.find_last_not_of(" ") + 1);
contentLength = std::stoll(lengthStr);
}
}

if (targetHost.empty() || targetPath.empty() || contentLength <= 0) {
std::string response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 51\r\n\r\nMissing required headers: Host, Target-Path, Content-Length";
send(socket, response.c_str(), response.length(), 0);
return;
}
      struct addrinfo hints = {}, *res;
      hints.ai_family       = AF_UNSPEC;
      hints.ai_socktype     = SOCK_STREAM;

      // Parse URL to get host and path
      std::string host, path, port = "80";
      bool        isConnect = request.substr(0, firstSpace) == "CONNECT";

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
        size_t hostEnd   = url.find('/', hostStart);
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
              const char *response = "HTTP/1.1 200 Connection established\r\n\r\n";
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
                  if (n <= 0)
                    break;
                  if (send(serverSock, buffer, n, 0) <= 0)
                    break;
                }

                if (FD_ISSET(serverSock, &readfds)) {
                  n = recv(serverSock, buffer, sizeof(buffer), 0);
                  if (n <= 0)
                    break;
                  if (send(socket, buffer, n, 0) <= 0)
                    break;
                }
              }
            } else {
              // For HTTP, we can cache the response
              std::string cacheKey    = host + path;
              auto        cachedEntry = cache->get(cacheKey);

              if (cachedEntry) {
                // Use cached response
                std::string response  = "HTTP/1.1 200 OK\r\n";
                response             += "Content-Type: " + cachedEntry->contentType + "\r\n";
                response             += "Content-Length: " + std::to_string(cachedEntry->content.length()) + "\r\n";
                if (!cachedEntry->contentEncoding.empty()) {
                  response += "Content-Encoding: " + cachedEntry->contentEncoding + "\r\n";
                }
                if (!cachedEntry->etag.empty()) {
                  response += "ETag: " + cachedEntry->etag + "\r\n";
                }
                response += "\r\n";
                response += cachedEntry->content;

                send(socket, response.c_str(), response.length(), 0);
                LOGI("Served from cache: %s", cacheKey.c_str());
              } else {
                // Forward the original request
                LOGI("Forwarding request to server: %.*s", (int)n, buffer);
                send(serverSock, buffer, n, 0);

                // Read and forward the response
                LOGI("=== Starting response handling ===");
                LOGI("About to read response from server...");
                std::string responseData;
                std::string contentType;
                std::string contentEncoding;
                std::string etag;
                ssize_t     contentLength = -1;
                bool        chunked       = false;
                LOGI("Variables initialized, starting read loop...");

                // Read response headers
                LOGI("Reading response headers...");
                LOGI("Starting to read response from server...");
                while ((n = recv(serverSock, buffer, sizeof(buffer), 0)) > 0) {
                  LOGI("Received %zd bytes from server", n);
                  LOGI("Raw received data: [%.*s]", (int)n, buffer);
                  responseData.append(buffer, n);
                  size_t headerEnd = responseData.find("\r\n\r\n");
                  LOGI("Current response data length: %zu", responseData.length());
                  if (headerEnd != std::string::npos) {
                    // Log the full response data we have so far
                    LOGI("Total response data received: %zu bytes", responseData.length());
                    LOGI("Headers end at position: %zu", headerEnd);
                    LOGI("Data after headers: %zu bytes", responseData.length() - (headerEnd + 4));
                    LOGI("Raw headers:\n%s", responseData.substr(0, headerEnd).c_str());

                    // Forward headers to client
                    send(socket, responseData.c_str(), responseData.length(), 0);

                    // Parse headers
                    LOGI("=== START OF RESPONSE HEADERS ===");
                    std::istringstream headerStream(responseData.substr(0, headerEnd));
                    std::string        line;
                    int                headerCount = 0;
                    bool               isFirstLine = true;
                    while (std::getline(headerStream, line)) {
                      // Remove \r if present at the end
                      if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                      }

                      if (isFirstLine) {
                        LOGI("Status line: '%s'", line.c_str());
                        isFirstLine = false;
                        continue;
                      }

                      if (line.empty()) {
                        LOGI("Found empty line (end of headers)");
                        continue;
                      }

                      // Log every header exactly as received
                      headerCount++;
                      LOGI("Raw header[%d]: '%s'", headerCount, line.c_str());

                      // Try to parse header name and value
                      size_t colonPos = line.find(':');
                      if (colonPos != std::string::npos) {
                        std::string name  = line.substr(0, colonPos);
                        std::string value = line.substr(colonPos + 1);
                        // Trim value
                        value.erase(0, value.find_first_not_of(" "));
                        value.erase(value.find_last_not_of(" ") + 1);
                        LOGI("Parsed header - Name: '%s', Value: '%s'", name.c_str(), value.c_str());

                        // Convert header name to lowercase for comparison
                        std::string lowerName = name;
                        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

                        if (lowerName == "content-length") {
                          contentLength = std::stoll(value);
                          LOGI("Parsed Content-Length: %zd", contentLength);
                        } else if (lowerName == "content-type") {
                          contentType = value;
                          LOGI("Found Content-Type: %s", contentType.c_str());
                        } else if (lowerName == "content-encoding") {
                          contentEncoding = value;
                          LOGI("Found Content-Encoding: %s", contentEncoding.c_str());
                        } else if (lowerName == "etag") {
                          etag = value;
                          LOGI("Found ETag: %s", etag.c_str());
                        }
                      }

                      // Start reading response body
                      LOGI("Headers parsed, starting to read response body...");
                      std::string responseBody;

                      // First, get any data after headers from the initial read
                      if (headerEnd + 4 < responseData.length()) {
                        responseBody = responseData.substr(headerEnd + 4);
                        LOGI("Got %zu bytes of body data from header buffer", responseBody.length());
                      }

                      // Continue reading until we have all the data
                      while (contentLength > 0 && responseBody.length() < contentLength) {
                        n = recv(serverSock, buffer, sizeof(buffer), 0);
                        if (n <= 0) {
                          if (n < 0) {
                            LOGE("Error reading response body: %s", strerror(errno));
                          }
                          break;
                        }
                        LOGI("Read %zd more bytes of body data", n);
                        responseBody.append(buffer, n);
                        LOGI("Total body size now: %zu/%zd bytes", responseBody.length(), contentLength);

                        // Forward this chunk to client
                        send(socket, buffer, n, 0);
                      }

                      LOGI("Finished reading response body. Total size: %zu bytes", responseBody.length());

                      // Cache the response if we got everything successfully
                      if (contentLength > 0 && responseBody.length() == contentLength) {
                        LOGI("Caching response - Content-Type: %s, ETag: %s, Size: %zu", contentType.c_str(), etag.c_str(),
                             responseBody.length());

                        DiskCache::CacheEntry entry;
                        entry.content     = responseBody;
                        entry.contentType = contentType;
                        entry.contentEncoding = contentEncoding;
                        entry.timestamp   = std::chrono::system_clock::now();
                        entry.etag        = etag;

                        if (cache) {
                          cache->put(cacheKey, entry);
                          LOGI("Successfully cached response");
                        } else {
                          LOGE("Cache object is null!");
                        }
                      } else {
                        LOGE("Not caching response - expected %zd bytes but got %zu", contentLength, responseBody.length());
                      }
                    }
                  }
                }
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
      close(socket);
    }
  }
}

extern "C" {

JNIEXPORT jint JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_startProxy(JNIEnv *env, jobject thiz, jstring configPath)
{
  if (proxyRunning) {
    return 0;
  }

  proxyRunning = true;

  // Initialize cache
  const char *cacheDir = env->GetStringUTFChars(configPath, nullptr);
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
Java_org_apache_trafficserver_test_TrafficServerProxyService_stopProxy(JNIEnv *env, jobject thiz)
{
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
Java_org_apache_trafficserver_test_TrafficServerProxyService_isProxyRunning(JNIEnv *env, jobject thiz)
{
  return proxyRunning;
}
}

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

#define LOG_TAG   "ProxyService"
#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))

struct RemapRule {
  std::string fromHost;
  std::string fromPath;
  std::string toHost;
  std::string toPath;
};

static std::atomic<bool>      proxyRunning{false};
static std::atomic<bool>      proxyReady{false};
static int                    serverSocket = -1;
static std::thread            proxyThread;
static std::vector<RemapRule> remapRules;
static std::mutex            proxyMutex;
static std::condition_variable proxyCondVar;

class DiskCache
{
public:
  struct CacheEntry {
    std::string                           content;
    std::string                           contentType;
    std::string                           contentEncoding;
    std::chrono::system_clock::time_point timestamp;
    std::string                           etag;
  };

  explicit DiskCache(const std::string &cacheDir) : cacheDir_(cacheDir)
  {
    std::filesystem::create_directories(cacheDir);
    LOGI("Initialized disk cache at: %s", cacheDir.c_str());
  }

  void
  put(const std::string &key, const CacheEntry &entry)
  {
    LOGI("DiskCache::put called with key: %s", key.c_str());
    std::string   path = getPath(key);
    std::ofstream file(path, std::ios::binary);
    if (file.is_open()) {
      try {
        // Write timestamp
        auto timestamp = std::chrono::system_clock::to_time_t(entry.timestamp);
        file.write(reinterpret_cast<const char *>(&timestamp), sizeof(timestamp));

        // Write content type length and content
        size_t len = entry.contentType.length();
        file.write(reinterpret_cast<const char *>(&len), sizeof(len));
        file.write(entry.contentType.c_str(), len);

        // Write content encoding length and content
        len = entry.contentEncoding.length();
        file.write(reinterpret_cast<const char *>(&len), sizeof(len));
        file.write(entry.contentEncoding.c_str(), len);

        // Write etag length and content
        len = entry.etag.length();
        file.write(reinterpret_cast<const char *>(&len), sizeof(len));
        file.write(entry.etag.c_str(), len);

        // Write content length and content
        len = entry.content.length();
        file.write(reinterpret_cast<const char *>(&len), sizeof(len));
        file.write(entry.content.c_str(), len);

        LOGI("Cached %s (%zu bytes)", key.c_str(), entry.content.length());
      } catch (const std::exception &e) {
        LOGE("Exception while writing cache: %s", e.what());
      }
    } else {
      LOGE("Failed to open cache file for writing: %s", path.c_str());
    }
  }

  std::optional<CacheEntry>
  get(const std::string &key)
  {
    std::string path = getPath(key);
    LOGI("Reading cache file: %s", path.c_str());
    std::ifstream file(path, std::ios::binary);
    if (file.is_open()) {
      try {
        CacheEntry entry;

        // Read timestamp
        std::time_t timestamp;
        file.read(reinterpret_cast<char *>(&timestamp), sizeof(timestamp));
        if (file.fail()) {
          LOGE("Failed to read timestamp");
          return std::nullopt;
        }
        entry.timestamp = std::chrono::system_clock::from_time_t(timestamp);

        // Read content type
        size_t len;
        file.read(reinterpret_cast<char *>(&len), sizeof(len));
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
        file.read(reinterpret_cast<char *>(&len), sizeof(len));
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
        file.read(reinterpret_cast<char *>(&len), sizeof(len));
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
        file.read(reinterpret_cast<char *>(&len), sizeof(len));
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
      } catch (const std::exception &e) {
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

  std::string
  getPath(const std::string &key) const
  {
    std::hash<std::string> hasher;
    size_t                 hash     = hasher(key);
    std::string            filename = std::to_string(hash);
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
  LOGI("ProxyLoop: Starting proxy loop thread...");
  LOGI("ProxyLoop: Creating socket...");
  serverSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (serverSocket < 0) {
    LOGE("Failed to create socket");
    proxyRunning = false;
    proxyReady = false;
    proxyCondVar.notify_all();
    return nullptr;
  }

  // Enable address reuse
  int opt = 1;
  LOGI("ProxyLoop: Setting socket options...");
  if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    LOGE("Failed to set socket options");
    close(serverSocket);
    serverSocket = -1;
    proxyRunning = false;
    proxyReady = false;
    proxyCondVar.notify_all();
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
    serverSocket = -1;
    proxyRunning = false;
    proxyReady = false;
    proxyCondVar.notify_all();
    return nullptr;
  }

  // Start listening
  if (listen(serverSocket, SOMAXCONN) < 0) {
    LOGE("Failed to listen on server socket");
    close(serverSocket);
    serverSocket = -1;
    proxyRunning = false;
    proxyReady = false;
    proxyCondVar.notify_all();
    return nullptr;
  }

  LOGI("Proxy server listening on port 8888");
  proxyReady = true;
  proxyCondVar.notify_all();

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
    LOGI("ProxyLoop: Creating new thread for client connection...");
    std::thread(handleClientConnection, clientSocket).detach();
    LOGI("ProxyLoop: Client thread detached, continuing to accept connections...");
  }

  LOGI("ProxyLoop: Exiting proxy loop thread, proxyRunning=%d", proxyRunning.load());
  close(serverSocket);
  return nullptr;
}

// Function to handle a client connection
static void
handleClientConnection(int socket)
{
  LOGI("handleClientConnection: Starting to handle client connection on socket %d", socket);
  std::vector<char> buffer(8192); // Start with 8KB but can grow
  size_t            totalBytes      = 0;
  bool              requestComplete = false;

  while (!requestComplete && totalBytes < 1048576) { // Max 1MB for safety
    ssize_t n = recv(socket, buffer.data() + totalBytes, buffer.size() - totalBytes - 1, 0);
    if (n <= 0) {
      if (n < 0) {
        LOGE("Error reading from socket: %s", strerror(errno));
      }
      close(socket);
      return;
    }

    totalBytes         += n;
    buffer[totalBytes]  = '\0';

    // Check if we have a complete request (ends with \r\n\r\n)
    if (strstr(buffer.data(), "\r\n\r\n")) {
      requestComplete = true;
    } else if (totalBytes >= buffer.size() - 1) {
      // Need more space
      buffer.resize(buffer.size() * 2);
    }
  }

  if (!requestComplete) {
    LOGE("Request too large or malformed");
    close(socket);
    LOGI("handleClientConnection: Closed socket %d due to malformed request", socket);
    return;
  }
  LOGI("handleClientConnection: Request complete, processing request on socket %d", socket);

  try {
    LOGI("Received request of %zu bytes", totalBytes);
    std::string     request(buffer.data(), totalBytes);
    std::string     method;
    std::string     url;
    std::string     host, path, port = "80";
    bool            isCacheLocal = false;
    std::string     contentType;
    std::string     targetHost;
    std::string     targetPath;
    ssize_t         contentLength = -1;
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    bool isConnect    = false;

    // Parse the request line to get method and URL
    size_t spacePos = request.find(' ');
    if (spacePos != std::string::npos) {
      method          = request.substr(0, spacePos);
      size_t urlStart = spacePos + 1;
      size_t urlEnd   = request.find(' ', urlStart);
      if (urlEnd != std::string::npos) {
        url = request.substr(urlStart, urlEnd - urlStart);
        LOGI("Parsed URL: %s", url.c_str());

        // Parse URL to get host and path
        if (method == "CONNECT") {
          isConnect       = true;
          host            = url; // For CONNECT, URL is host:port
          size_t colonPos = host.find(':');
          if (colonPos != std::string::npos) {
            port = host.substr(colonPos + 1);
            host = host.substr(0, colonPos);
          } else {
            port = "443";
          }
          path = "/";
        } else if (url.find("http://") == 0) {
          size_t hostStart = 7; // After "http://"
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
        } else if (url[0] == '/') {
          // Relative URL, get host from Host header
          path                 = url;
          size_t hostHeaderPos = request.find("\r\nHost: ");
          if (hostHeaderPos != std::string::npos) {
            hostHeaderPos  += 8; // Skip "\r\nHost: "
            size_t hostEnd  = request.find("\r\n", hostHeaderPos);
            if (hostEnd != std::string::npos) {
              host = request.substr(hostHeaderPos, hostEnd - hostHeaderPos);
              // Check for port in host header
              size_t colonPos = host.find(':');
              if (colonPos != std::string::npos) {
                port = host.substr(colonPos + 1);
                host = host.substr(0, colonPos);
              }
            }
          }
        } else {
          LOGE("First Invalid URL format: %s", url.c_str());
          close(socket);
          return;
        }

        LOGI("Parsed request - Host: %s, Port: %s, Path: %s", host.c_str(), port.c_str(), path.c_str());
      } else {
        LOGE("Invalid request format");
        close(socket);
        LOGI("handleClientConnection: Closed socket %d due to invalid request format", socket);
        return;
      }
    } else {
      LOGE("Invalid request format");
      close(socket);
      LOGI("handleClientConnection: Closed socket %d due to invalid request format", socket);
      return;
    }

    LOGI("handleClientConnection: Starting to parse headers on socket %d", socket);
    // Parse headers into a map for easier access
    std::map<std::string, std::string> headers;
    size_t headerStart = request.find("\r\n") + 2;
    while (headerStart < request.length()) {
        size_t headerEnd = request.find("\r\n", headerStart);
        if (headerEnd == std::string::npos)
            break;

        std::string header = request.substr(headerStart, headerEnd - headerStart);
        size_t colonPos = header.find(':');
        if (colonPos != std::string::npos) {
            std::string name = header.substr(0, colonPos);
            std::string value = header.substr(colonPos + 1);
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" "));
            value.erase(value.find_last_not_of(" ") + 1);
            headers[name] = value;
        }
        headerStart = headerEnd + 2;
    }

    // Process standard headers
    if (headers.find("Content-Type") != headers.end()) {
        contentType = headers["Content-Type"];
    }
    if (headers.find("Content-Length") != headers.end()) {
        contentLength = std::stoll(headers["Content-Length"]);
    }
    if (url[0] == '/') {
        // For relative URLs, get host from Host header
        if (headers.find("Host") != headers.end()) {
            host = headers["Host"];
            // Check for port in host header
            size_t colonPos = host.find(':');
            if (colonPos != std::string::npos) {
                port = host.substr(colonPos + 1);
                host = host.substr(0, colonPos);
            }
        }
    }

    // Check if this is a cache population request
    if (method == "POST" && url == "/cache/populate") {
        std::string targetHost = headers["Host"];
        std::string targetPath = headers["Target-Path"];
        std::string requestBody;

        // Extract request body
        size_t bodyStart = request.find("\r\n\r\n");
        if (bodyStart != std::string::npos) {
            requestBody = request.substr(bodyStart + 4);
        }

        // Create cache entry
        if (!targetHost.empty() && !targetPath.empty()) {
            std::string cacheKey = targetHost + targetPath;
            DiskCache::CacheEntry entry;
            entry.content = requestBody;
            entry.contentType = contentType;
            entry.timestamp = std::chrono::system_clock::now();

            if (cache) {
                cache->put(cacheKey, entry);
                // Send success response
                std::string response = "HTTP/1.1 200 OK\r\n"
                                     "Content-Length: 0\r\n"
                                     "Connection: close\r\n\r\n";
                send(socket, response.c_str(), response.length(), 0);
                close(socket);
                return;
            }
        }

        // If we get here, something went wrong
        std::string response = "HTTP/1.1 400 Bad Request\r\n"
                             "Content-Length: 0\r\n"
                             "Connection: close\r\n\r\n";
        send(socket, response.c_str(), response.length(), 0);
        close(socket);
        return;
    }
    // For HTTP requests, check cache first before any network operations
    if (!isConnect) {
      std::string cacheKey = host + path;
      LOGI("Checking cache for key: %s", cacheKey.c_str());
      auto cachedEntry = cache->get(cacheKey);

      if (cachedEntry) {
        // Use cached response
        std::string response = "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: " + cachedEntry->contentType + "\r\n";
        if (!cachedEntry->contentEncoding.empty()) {
          response += "Content-Encoding: " + cachedEntry->contentEncoding + "\r\n";
        }
        if (!cachedEntry->etag.empty()) {
          response += "ETag: " + cachedEntry->etag + "\r\n";
        }
        response += "Content-Length: " + std::to_string(cachedEntry->content.length()) + "\r\n";
        response += "\r\n";
        response += cachedEntry->content;

        LOGI("Sending cached response headers (%zu bytes)", response.length() - cachedEntry->content.length());
        ssize_t sent = send(socket, response.c_str(), response.length(), 0);
        if (sent < 0) {
          LOGE("Failed to send cached response: %s", strerror(errno));
        } else {
          LOGI("Successfully sent %zd bytes from cache for key: %s", sent, cacheKey.c_str());
        }
        close(socket);
        LOGI("handleClientConnection: Closed socket %d after serving cached response", socket);
        return;
      }
    }

    // If not in cache or HTTPS, proceed with network operations
    LOGI("Connecting to %s:%s", host.c_str(), port.c_str());

    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0) {
        int serverSock = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (serverSock >= 0) {
          if (connect(serverSock, res->ai_addr, res->ai_addrlen) == 0) {
            if (isConnect) {
              // For HTTPS, just tunnel the connection
              const char *response = "HTTP/1.1 200 Connection established\r\n\r\n";
              send(socket, response, strlen(response), 0);

              fd_set            readfds;
              struct timeval    tv;
              std::vector<char> buffer(8192);
              ssize_t           n;
              while (true) {
                FD_ZERO(&readfds);
                FD_SET(socket, &readfds);
                FD_SET(serverSock, &readfds);
                tv.tv_sec  = 1;
                tv.tv_usec = 0;

                int maxfd = std::max(socket, serverSock) + 1;
                int ready = select(maxfd, &readfds, nullptr, nullptr, &tv);
                if (ready < 0)
                  break;

                if (FD_ISSET(socket, &readfds)) {
                  ssize_t n = recv(socket, buffer.data(), buffer.size() - 1, 0);
                  if (n <= 0)
                    break;
                  if (send(serverSock, buffer.data(), n, 0) <= 0)
                    break;
                }

                if (FD_ISSET(serverSock, &readfds)) {
                  n = recv(serverSock, buffer.data(), buffer.size() - 1, 0);
                  if (n <= 0)
                    break;
                  if (send(socket, buffer.data(), n, 0) <= 0)
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
                response += "Content-Length: " + std::to_string(cachedEntry->content.length()) + "\r\n";
                response += "\r\n";
                response += cachedEntry->content;

                LOGI("Sending cached response headers (%zu bytes)", response.length() - cachedEntry->content.length());
                ssize_t sent = send(socket, response.c_str(), response.length(), 0);
                if (sent < 0) {
                  LOGE("Failed to send cached response: %s", strerror(errno));
                } else {
                  LOGI("Successfully sent %zd bytes from cache for key: %s", sent, cacheKey.c_str());
                  LOGI("handleClientConnection: Closing socket %d after serving cached response", socket);
                  close(socket);
                  return;
                }
              } else {
                // Forward the original request
                LOGI("Forwarding request to server: %.*s", (int)buffer.size(), buffer.data());
                send(serverSock, buffer.data(), buffer.size(), 0);

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
                ssize_t n;
                while ((n = recv(serverSock, buffer.data(), buffer.size() - 1, 0)) > 0) {
                  LOGI("Received %zd bytes from server", n);
                  LOGI("Raw received data: [%.*s]", (int)n, buffer.data());
                  responseData.append(buffer.data(), n);
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
                        ssize_t n = recv(serverSock, buffer.data(), buffer.size() - 1, 0);
                        if (n <= 0) {
                          if (n < 0) {
                            LOGE("Error reading response body: %s", strerror(errno));
                          }
                          break;
                        }
                        LOGI("Read %zd more bytes of body data", n);
                        responseBody.append(buffer.data(), n);
                        LOGI("Total body size now: %zu/%zd bytes", responseBody.length(), contentLength);

                        // Forward this chunk to client
                        send(socket, buffer.data(), n, 0);
                      }

                      LOGI("Finished reading response body. Total size: %zu bytes", responseBody.length());

                      // Cache the response if we got everything successfully
                      if (contentLength > 0 && responseBody.length() == contentLength) {
                        LOGI("Caching response - Content-Type: %s, ETag: %s, Size: %zu", contentType.c_str(), etag.c_str(),
                             responseBody.length());

                        DiskCache::CacheEntry entry;
                        entry.content         = responseBody;
                        entry.contentType     = contentType;
                        entry.contentEncoding = contentEncoding;
                        entry.timestamp       = std::chrono::system_clock::now();
                        entry.etag            = etag;

                        if (cache) {
                          LOGI("About to cache response for key: %s", cacheKey.c_str());
                          cache->put(cacheKey, entry);
                          LOGI("Successfully cached response for key: %s", cacheKey.c_str());
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
          } // End of if (connect)
          close(serverSock);
        } // End of if (serverSock >= 0)
        freeaddrinfo(res);
    } else {
        LOGE("Failed to resolve host");
    }
    close(socket);
  } catch (const std::exception &e) {
    LOGE("Exception in handleClientConnection: %s", e.what());
    LOGI("handleClientConnection: Exception stack trace:");
    LOGE("%s", e.what());
    close(socket);
    LOGI("handleClientConnection: Closed socket %d due to exception", socket);
  } catch (...) {
    LOGE("Unknown exception in handleClientConnection");
    close(socket);
    LOGI("handleClientConnection: Closed socket %d due to unknown exception", socket);
  }
  LOGI("handleClientConnection: Finished handling client connection on socket %d", socket);
}

extern "C" {

JNIEXPORT jint JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_startProxy(JNIEnv *env, jobject thiz, jstring configPath)
{
  if (proxyRunning) {
    return 0;
  }

  proxyRunning = true;
  proxyReady = false;

  // Initialize cache
  const char *cacheDir = env->GetStringUTFChars(configPath, nullptr);
  if (cacheDir) {
    cache = std::make_unique<DiskCache>(std::string(cacheDir) + "/cache");
    env->ReleaseStringUTFChars(configPath, cacheDir);
  }

  // Start proxy thread
  proxyThread = std::thread(proxyLoop, nullptr);
  proxyThread.detach();

  // Wait for proxy to be ready or fail
  {
    std::unique_lock<std::mutex> lock(proxyMutex);
    if (proxyCondVar.wait_for(lock, std::chrono::seconds(5), []{ return proxyReady || !proxyRunning; })) {
      // If we're here, either proxy is ready or it failed to start
      return proxyReady ? 0 : -1;
    } else {
      // Timeout waiting for proxy to start
      LOGE("Timeout waiting for proxy to start");
      proxyRunning = false;
      return -1;
    }
  }
}

JNIEXPORT void JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_stopProxy(JNIEnv *env, jobject thiz)
{
  if (!proxyRunning) {
    return;
  }

  proxyRunning = false;
  proxyReady = false;
  if (serverSocket >= 0) {
    shutdown(serverSocket, SHUT_RDWR);
    close(serverSocket);
    serverSocket = -1;
  }
  // Give some time for the socket to fully close
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

JNIEXPORT jboolean JNICALL
Java_org_apache_trafficserver_test_TrafficServerProxyService_isProxyRunning(JNIEnv *env, jobject thiz)
{
  return proxyRunning && proxyReady;
}

} // End of extern "C"

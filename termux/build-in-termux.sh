#!/data/data/com.termux/files/usr/bin/bash

# Exit on error
set -e

# Update package lists
pkg update -y

# Install required packages
pkg install -y \
    autoconf \
    automake \
    bison \
    clang \
    curl \
    git \
    libtool \
    make \
    openssl-dev \
    pcre2-dev \
    pkg-config \
    python \
    libexpat \
    libxml2-dev \
    zlib-dev \
    libcap-dev

# Create directories
mkdir -p ~/trafficserver
cd ~/trafficserver

# Clone Traffic Server (if not exists)
if [ ! -d "trafficserver" ]; then
    git clone https://github.com/apache/trafficserver.git
    cd trafficserver
else
    cd trafficserver
    git pull
fi

# Configure build
autoreconf -if
./configure \
    --prefix=$HOME/ts \
    --enable-experimental-plugins \
    --disable-tests \
    --disable-docs \
    --enable-wccp \
    CFLAGS="-I$PREFIX/include" \
    CXXFLAGS="-I$PREFIX/include" \
    LDFLAGS="-L$PREFIX/lib"

# Build and install
make -j4
make install

# Create basic configuration
mkdir -p $HOME/ts/etc/trafficserver
cat > $HOME/ts/etc/trafficserver/records.yaml << EOL
# Basic Traffic Server configuration
proxy:
  config:
    http_accept_no_activity_timeout: 120
    http_connect_attempts_max_retries: 3
    http_connect_attempts_timeout: 30
    proxy_protocol_enabled: 0
    server_ports: 8080
    http_server_session_sharing_pool: "both"
    http_cache_http: 1
    cache_clustering_enabled: 0
    proxy_protocol_enabled: 0
EOL

# Create remap configuration
cat > $HOME/ts/etc/trafficserver/remap.config << EOL
# Basic remap rule
map http://example.com/ http://localhost:8080/
reverse_map http://localhost:8080/ http://example.com/
EOL

# Create startup script
cat > ~/start-ts.sh << EOL
#!/data/data/com.termux/files/usr/bin/bash
export TS_ROOT=\$HOME/ts
cd \$TS_ROOT
./bin/traffic_cop &
EOL

chmod +x ~/start-ts.sh

echo "Traffic Server has been built and installed to ~/ts"
echo "To start Traffic Server, run: ~/start-ts.sh"
echo "To test, use: curl -x http://localhost:8080 http://example.com"

# Building Apache Traffic Server in Termux

This guide explains how to build and run Apache Traffic Server on Android using Termux.

## Prerequisites

1. Install Termux from F-Droid or Google Play Store
2. Give Termux storage permission:
   ```bash
   termux-setup-storage
   ```

## Installation Steps

1. Copy the build script to your Android device:
   ```bash
   # On your computer
   adb push build-in-termux.sh /sdcard/Download/
   
   # In Termux
   cp /sdcard/Download/build-in-termux.sh ~/
   chmod +x ~/build-in-termux.sh
   ```

2. Run the build script:
   ```bash
   ./build-in-termux.sh
   ```
   This will:
   - Install all required dependencies
   - Clone and build Traffic Server
   - Create basic configuration
   - Set up startup script

3. Start Traffic Server:
   ```bash
   ~/start-ts.sh
   ```

## Testing

1. Basic proxy test:
   ```bash
   curl -x http://localhost:8080 http://example.com
   ```

2. Check logs:
   ```bash
   tail -f ~/ts/var/log/trafficserver/error.log
   ```

3. View status:
   ```bash
   ~/ts/bin/traffic_ctl status
   ```

## Configuration

Main configuration files are in `~/ts/etc/trafficserver/`:
- `records.yaml`: Main configuration
- `remap.config`: URL remapping rules
- `ip_allow.yaml`: Access control

## Troubleshooting

1. If Traffic Server fails to start:
   ```bash
   # Check if process is running
   ps aux | grep traffic
   
   # Check logs
   tail -f ~/ts/var/log/trafficserver/error.log
   
   # Check port availability
   netstat -tulpn | grep 8080
   ```

2. If proxy doesn't work:
   - Verify Traffic Server is running
   - Check Android's network permissions
   - Ensure no other proxy is running on port 8080

3. Memory issues:
   - Adjust `proxy.config.cache.ram_cache.size` in records.yaml
   - Reduce number of worker threads

## Limitations

1. Android Security:
   - Some Android versions may restrict network access
   - Root is not required but may help with certain features

2. Performance:
   - Memory usage is limited by Termux
   - Storage performance depends on device

3. Battery Usage:
   - Running a proxy server continuously will impact battery life
   - Consider using wake locks if needed

## Updating

To update Traffic Server:
```bash
cd ~/trafficserver/trafficserver
git pull
make clean
./build-in-termux.sh
```

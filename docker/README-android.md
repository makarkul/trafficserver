# Building Apache Traffic Server for Android

This guide explains how to build Apache Traffic Server (ATS) for Android devices using cross-compilation.

## Prerequisites

- Docker installed on your build machine
- Android device with root access (or a rooted Android emulator)
- ADB (Android Debug Bridge) installed on your development machine

## Building

1. Build the Android package:
   ```bash
   cd docker
   docker-compose -f docker-compose.android.yml up --build
   ```
   This will create `trafficserver-android-arm64-v8a.tar.gz` in the `android-build` directory.

2. Transfer to Android device:
   ```bash
   # Push the package to the device
   adb push android-build/trafficserver-android-arm64-v8a.tar.gz /data/local/tmp/
   
   # Shell into the device
   adb shell
   
   # Extract (on the Android device)
   su
   cd /data/local/tmp
   tar xzf trafficserver-android-arm64-v8a.tar.gz -C /data/local/trafficserver
   ```

## Running on Android

1. Set up the environment:
   ```bash
   # On the Android device (as root)
   export TS_ROOT=/data/local/trafficserver
   export PATH=$PATH:$TS_ROOT/bin
   ```

2. Start Traffic Server:
   ```bash
   # Start the traffic manager
   traffic_manager &
   
   # Verify it's running
   ps | grep traffic
   ```

3. Test the proxy:
   ```bash
   # Using curl on Android
   curl -x http://localhost:8080 http://example.com
   ```

## Configuration

The configuration files are in `/data/local/trafficserver/etc/trafficserver/`:
- `records.yaml`: Main configuration
- `remap.config`: URL remapping rules
- `ip_allow.yaml`: Access control

## Known Limitations

1. SELinux/Android Security:
   - Device must be rooted
   - SELinux might need to be set to permissive mode
   - Some Android security features might restrict network access

2. Resource Usage:
   - Android's memory management might kill the service
   - Cache size should be limited on devices with limited storage

3. Performance:
   - CPU frequency scaling might affect performance
   - I/O performance depends on device storage type

## Debugging

1. Check logs:
   ```bash
   tail -f /data/local/trafficserver/var/log/trafficserver/error.log
   ```

2. Monitor process:
   ```bash
   top | grep traffic
   ```

3. Network connectivity:
   ```bash
   netstat -an | grep 8080
   ```

## Security Considerations

1. Running as root:
   - Traffic Server requires root privileges on Android
   - Consider security implications in production use

2. Network Access:
   - Configure `ip_allow.yaml` carefully
   - Consider using VPN integration for non-root operation

3. Storage:
   - Cache directory should be in a secure location
   - Regular cleanup might be needed

## Troubleshooting

1. If Traffic Server fails to start:
   - Check SELinux status: `getenforce`
   - Verify permissions: `ls -l /data/local/trafficserver`
   - Check system logs: `logcat | grep traffic`

2. If proxy doesn't work:
   - Verify port is open: `netstat -an | grep 8080`
   - Check firewall rules
   - Verify network permissions

3. Performance issues:
   - Monitor CPU: `top`
   - Check storage I/O: `iostat`
   - Adjust cache size in `records.yaml`

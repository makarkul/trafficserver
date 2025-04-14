package org.apache.trafficserver.test

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.net.VpnService
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.util.Log
import androidx.core.app.NotificationCompat
import java.io.FileInputStream
import java.io.FileOutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import kotlin.concurrent.thread
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory

class TrafficServerVpnService : VpnService() {
    companion object {
        private const val TAG = "TrafficServerVPN"
        private const val NOTIFICATION_CHANNEL_ID = "VPN_SERVICE"
        private const val NOTIFICATION_ID = 1
        private const val PROXY_ADDRESS = "127.0.0.1"
        private const val PROXY_PORT = 8888

        // IP header constants
        private const val IP_HEADER_SIZE = 20
        private const val PROTOCOL_TCP = 6
        
        // TCP header constants
        private const val TCP_HEADER_SIZE = 20
        private const val TCP_FLAG_SYN = 0x02
        private const val TCP_FLAG_ACK = 0x10
    }

    // Track TCP connections
    private data class TcpConnection(val srcPort: Int, val dstPort: Int)
    private val tcpConnections = mutableSetOf<TcpConnection>()

    private fun getIpHeaderLength(packet: ByteArray): Int {
        return (packet[0].toInt() and 0x0F) * 4 // IHL field * 4 bytes
    }

    private fun getTcpHeaderLength(packet: ByteArray, ipHeaderLength: Int): Int {
        // Data offset field is top 4 bits, multiply by 4 to get bytes
        return ((packet[ipHeaderLength + 12].toInt() and 0xF0) shr 4) * 4
    }

    private fun extractHttpRequest(packet: ByteArray, length: Int): ByteArray? {
        if (length < IP_HEADER_SIZE + TCP_HEADER_SIZE) {
            Log.d(TAG, "Packet too small for IP+TCP headers")
            return null
        }

        // Check if it's a TCP packet
        val protocol = packet[9].toInt() and 0xFF
        if (protocol != PROTOCOL_TCP) {
            Log.d(TAG, "Not a TCP packet (protocol=$protocol)")
            return null
        }

        // Get source and destination ports
        val srcPort = ((packet[20].toInt() and 0xFF) shl 8) or (packet[21].toInt() and 0xFF)
        val dstPort = ((packet[22].toInt() and 0xFF) shl 8) or (packet[23].toInt() and 0xFF)
        val tcpFlags = packet[33].toInt() and 0xFF

        // Handle TCP connection establishment
        val connection = TcpConnection(srcPort, dstPort)
        if ((tcpFlags and TCP_FLAG_SYN) != 0) {
            if ((tcpFlags and TCP_FLAG_ACK) == 0) {
                // SYN packet - new connection
                Log.d(TAG, "New TCP connection: $srcPort -> $dstPort")
                tcpConnections.add(connection)
            }
            return null // Skip SYN packets
        }

        val ipHeaderLength = getIpHeaderLength(packet)
        if (ipHeaderLength < IP_HEADER_SIZE) {
            Log.d(TAG, "Invalid IP header length: $ipHeaderLength")
            return null
        }

        val tcpHeaderLength = getTcpHeaderLength(packet, ipHeaderLength)
        if (tcpHeaderLength < TCP_HEADER_SIZE) {
            Log.d(TAG, "Invalid TCP header length: $tcpHeaderLength")
            return null
        }

        val totalHeaderLength = ipHeaderLength + tcpHeaderLength
        if (length <= totalHeaderLength) {
            Log.d(TAG, "No payload data in packet")
            return null
        }

        // Extract HTTP payload
        val payloadLength = length - totalHeaderLength
        val payload = ByteArray(payloadLength)
        System.arraycopy(packet, totalHeaderLength, payload, 0, payloadLength)

        // Check if it looks like an HTTP request
        val payloadStr = String(payload, 0, minOf(8, payloadLength))
        if (!payloadStr.startsWith("GET ") && 
            !payloadStr.startsWith("POST ") && 
            !payloadStr.startsWith("HEAD ") && 
            !payloadStr.startsWith("PUT ") && 
            !payloadStr.startsWith("DELETE ") && 
            !payloadStr.startsWith("CONNECT ") && 
            !payloadStr.startsWith("OPTIONS ") && 
            !payloadStr.startsWith("TRACE ")) {
            Log.d(TAG, "Not an HTTP request: $payloadStr")
            return null
        }

        Log.i(TAG, "Found HTTP request: ${payloadStr.substringBefore('\n')}")
        return payload
    }

    private var vpnInterface: ParcelFileDescriptor? = null
    private var isRunning = false
    private val socketProtectionReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: android.content.Context?, intent: android.content.Intent?) {
            if (intent?.action == "org.apache.trafficserver.PROTECT_SOCKET") {
                val fd = intent.getIntExtra("socket_fd", -1)
                if (fd != -1) {
                    Log.i(TAG, "Protecting socket with fd: $fd")
                    protect(fd)
                }
            }
        }
    }
    private val binder = LocalBinder()


    private data class IPHeader(
        val headerLength: Int,
        val protocol: Int,
        val destPort: Int
    )

    inner class LocalBinder : Binder() {
        val service: TrafficServerVpnService
            get() = this@TrafficServerVpnService
    }

    override fun onBind(intent: Intent): IBinder? {
        return binder
    }

    override fun onCreate() {
        super.onCreate()
        // Register for socket protection broadcasts
        registerReceiver(socketProtectionReceiver, android.content.IntentFilter("org.apache.trafficserver.PROTECT_SOCKET"))
        Log.i(TAG, "VPN Service created")
        createNotificationChannel()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                NOTIFICATION_CHANNEL_ID,
                "TrafficServer VPN Service",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Keeps TrafficServer VPN running"
            }
            val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            notificationManager.createNotificationChannel(channel)
        }
    }

    private fun createNotification(): Notification {
        return NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
            .setContentTitle("TrafficServer VPN")
            .setContentText("VPN service is running")
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.i(TAG, "VPN Service onStartCommand called")
        if (isRunning) {
            Log.i(TAG, "VPN Service already running")
            return START_STICKY
        }

        // Start as foreground service
        startForeground(NOTIFICATION_ID, createNotification())

        thread(start = true) {
            try {
                establishVpn()
                startVpn()
            } catch (e: Exception) {
                Log.e(TAG, "Error in VPN thread", e)
                cleanup()
            }
        }

        return START_STICKY
    }

    private fun establishVpn() {
        Log.i(TAG, "Establishing VPN connection")
        Log.i(TAG, "Package name: $packageName")
        
        // Create a new builder for VPN interface
        val builder = Builder()
            .addAddress("10.0.0.2", 24)  // Virtual IP for our VPN with subnet mask
            .addRoute("0.0.0.0", 0)      // Route all IPv4 traffic through VPN
            .addDnsServer("8.8.8.8")     // Use Google DNS
            .setSession("TrafficServer VPN")
            .setMtu(1500)
            .allowFamily(android.system.OsConstants.AF_INET)  // Allow IPv4 only
            .allowFamily(android.system.OsConstants.AF_INET6) // Allow IPv6 too
            .setBlocking(true)
            .allowBypass()  // Allow apps to bypass VPN

        try {
            // Allow traffic to our proxy and essential system apps
            Log.i(TAG, "Adding allowed applications...")
            builder.addAllowedApplication(packageName)
            builder.addAllowedApplication("org.jellyfin.mobile")
            Log.i(TAG, "Successfully added allowed applications")
        } catch (e: Exception) {
            Log.e(TAG, "Error adding allowed applications", e)
            throw e  // Let's see what's failing
        }
        
        // Establish the VPN interface
        Log.i(TAG, "Establishing VPN interface...")
        vpnInterface = builder.establish()
        if (vpnInterface == null) {
            Log.e(TAG, "Failed to establish VPN interface")
            throw IllegalStateException("Failed to establish VPN connection")
        }
        Log.i(TAG, "VPN interface established successfully")
        
        isRunning = true
        Log.i(TAG, "VPN interface established")
    }

    private fun startVpn() {
        isRunning = true
        Log.i(TAG, "Starting VPN tunnel")
        
        try {
            val vpnInput = FileInputStream(vpnInterface?.fileDescriptor)
            val vpnOutput = FileOutputStream(vpnInterface?.fileDescriptor)
            val packet = ByteBuffer.allocate(32767)
            
            while (isRunning) {
                // Clear the packet buffer
                packet.clear()
                
                // Read incoming packet from VPN interface
                val length = vpnInput.read(packet.array())
                if (length <= 0) continue
                
                // Log packet details
                if (length >= 24) { // Minimum size for TCP/UDP header
                    val protocol = packet[9].toInt() and 0xFF
                    val srcPort = ((packet[20].toInt() and 0xFF) shl 8) or (packet[21].toInt() and 0xFF)
                    val destPort = ((packet[22].toInt() and 0xFF) shl 8) or (packet[23].toInt() and 0xFF)
                    Log.d(TAG, "Received packet: protocol=$protocol, srcPort=$srcPort, destPort=$destPort")
                }
                
                Log.d(TAG, "Read $length bytes from VPN interface")
                
                // Forward all traffic to our proxy
                val proxySocket = Socket(PROXY_ADDRESS, PROXY_PORT)
                protect(proxySocket) // Prevent VPN from capturing proxy traffic
                
                // Extract HTTP request from packet
                val httpRequest = extractHttpRequest(packet.array(), length)
                if (httpRequest == null) {
                    Log.d(TAG, "Packet is not an HTTP request, skipping")
                    proxySocket.close()
                    continue
                }

                Log.i(TAG, "Forwarding HTTP request to proxy, length: ${httpRequest.size}")
                proxySocket.getOutputStream().write(httpRequest)
                proxySocket.getOutputStream().flush()
                
                // Read response from proxy
                val response = ByteArray(32767)
                val responseLength = proxySocket.getInputStream().read(response)
                
                if (responseLength > 0) {
                    // Write response back to VPN
                    vpnOutput.write(response, 0, responseLength)
                    Log.d(TAG, "Wrote $responseLength bytes back to VPN")
                }
                
                proxySocket.close()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error in VPN tunnel", e)
            cleanup()
        }
    }
    private fun parseIPHeader(packet: ByteArray, length: Int): IPHeader {
        val version = (packet[0].toInt() and 0xF0) shr 4
        Log.d(TAG, "IP version: $version")
        if (length < 20) return IPHeader(20, 0, 0) // Invalid packet
        
        val headerLength = (packet[0].toInt() and 0xF) * 4
        val protocol = packet[9].toInt() and 0xFF
        
        var destPort = 0
        if (length >= headerLength + 2) {
            destPort = ((packet[headerLength + 2].toInt() and 0xFF) shl 8) or
                      (packet[headerLength + 3].toInt() and 0xFF)
        }
        
        return IPHeader(headerLength, protocol, destPort)
    }
    
    private fun swapAddresses(packet: ByteArray) {
        // Swap source and destination IP addresses
        for (i in 0..3) {
            val temp = packet[12 + i]
            packet[12 + i] = packet[16 + i]
            packet[16 + i] = temp
        }
        
        // Swap source and destination ports if TCP/UDP
        if (packet[9].toInt() == 6 || packet[9].toInt() == 17) { // TCP or UDP
            val headerLength = (packet[0].toInt() and 0xF) * 4
            for (i in 0..1) {
                val temp = packet[headerLength + i]
                packet[headerLength + i] = packet[headerLength + 2 + i]
                packet[headerLength + 2 + i] = temp
            }
        }
    }

    private fun cleanup() {
        isRunning = false
        
        if (vpnInterface != null) {
            Log.i(TAG, "Closing VPN interface")
            vpnInterface?.close()
            vpnInterface = null
        }
    }

    override fun onDestroy() {
        try {
            unregisterReceiver(socketProtectionReceiver)
        } catch (e: Exception) {
            Log.e(TAG, "Error unregistering receiver", e)
        }
        super.onDestroy()
        cleanup()
    }
}

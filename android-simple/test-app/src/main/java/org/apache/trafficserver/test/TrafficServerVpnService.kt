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
import java.net.Socket
import java.nio.ByteBuffer
import kotlin.concurrent.thread

class TrafficServerVpnService : VpnService() {
    companion object {
        private const val TAG = "TrafficServerVPN"
        private const val NOTIFICATION_CHANNEL_ID = "VPN_SERVICE"
        private const val NOTIFICATION_ID = 1
        private const val PROXY_ADDRESS = "127.0.0.1"
        private const val PROXY_PORT = 8888
        private const val BUFFER_SIZE = 32767
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
        val vpnInput = FileInputStream(vpnInterface?.fileDescriptor)
        val vpnOutput = FileOutputStream(vpnInterface?.fileDescriptor)
        val packet = ByteBuffer.allocate(BUFFER_SIZE)

        try {
            while (isRunning) {
                packet.clear()
                
                // Read incoming packet from VPN interface
                val length = vpnInput.read(packet.array())
                if (length <= 0) continue
                
                Log.d(TAG, "Read $length bytes from VPN interface")
                
                // Forward all traffic to our proxy
                val proxySocket = Socket(PROXY_ADDRESS, PROXY_PORT)
                protect(proxySocket) // Prevent VPN from capturing proxy traffic
                
                // Forward the entire packet to proxy
                proxySocket.getOutputStream().write(packet.array(), 0, length)
                proxySocket.getOutputStream().flush()
                
                // Read response from proxy
                val response = ByteArray(BUFFER_SIZE)
                val responseLength = proxySocket.getInputStream().read(response)
                
                if (responseLength > 0) {
                    // Write response back to VPN
                    vpnOutput.write(response, 0, responseLength)
                }
                
                proxySocket.close()
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error in VPN tunnel", e)
            cleanup()
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

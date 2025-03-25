package org.apache.trafficserver.test

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import java.io.File
import android.os.IBinder
import androidx.core.app.NotificationCompat

class TrafficServerProxyService : Service() {
    companion object {
        private const val NOTIFICATION_CHANNEL_ID = "TrafficServerProxy"
        private const val NOTIFICATION_ID = 1

        init {
            System.loadLibrary("trafficserver_test")
        }
    }

    private external fun startProxy(configPath: String): Int
    private external fun stopProxy()
    private external fun isProxyRunning(): Boolean

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = createNotification()
        startForeground(NOTIFICATION_ID, notification)

        val configPath = getConfigPath()
        startProxy(configPath)

        return START_STICKY
    }

    override fun onDestroy() {
        stopProxy()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            NOTIFICATION_CHANNEL_ID,
            "Traffic Server Proxy",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Running Traffic Server proxy service"
        }

        val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        notificationManager.createNotificationChannel(channel)
    }

    private fun createNotification(): Notification {
        return NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
            .setContentTitle("Traffic Server Proxy")
            .setContentText("Proxy service is running")
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun getConfigPath(): String {
        // Create config directory if it doesn't exist
        val configDir = getDir("config", Context.MODE_PRIVATE)
        if (!configDir.exists()) {
            configDir.mkdirs()
        }

        // Create remap.config if it doesn't exist
        val remapConfig = File(configDir, "remap.config")
        if (!remapConfig.exists()) {
            remapConfig.writeText("""
                # Basic remap rules
                map http://example.com/ http://info.cern.ch/
                map https://example.com/ https://api.github.com/zen
            """.trimIndent())
        }

        return configDir.absolutePath
    }
}

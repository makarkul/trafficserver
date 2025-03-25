package org.apache.trafficserver.android

import android.app.*
import android.content.Context
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import kotlinx.coroutines.*
import java.io.IOException
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

class SimpleProxyService : Service() {
    private val running = AtomicBoolean(false)
    private var serverSocket: ServerSocket? = null
    private val scope = CoroutineScope(Dispatchers.IO + Job())
    
    companion object {
        private const val NOTIFICATION_CHANNEL_ID = "SimpleProxy"
        private const val NOTIFICATION_ID = 1
        private const val DEFAULT_PORT = 8080
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIFICATION_ID, createNotification())
        startProxy()
        return START_STICKY
    }

    override fun onDestroy() {
        stopProxy()
        scope.cancel()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun startProxy() {
        if (running.get()) return
        
        android.util.Log.d("SimpleProxy", "Starting proxy server on port $DEFAULT_PORT")
        running.set(true)
        scope.launch {
            try {
                serverSocket = ServerSocket(DEFAULT_PORT)
                android.util.Log.d("SimpleProxy", "Server socket created and listening")
                while (running.get()) {
                    try {
                        val clientSocket = serverSocket?.accept() ?: break
                        android.util.Log.d("SimpleProxy", "New client connection from ${clientSocket.inetAddress}:${clientSocket.port}")
                        launch { handleConnection(clientSocket) }
                    } catch (e: IOException) {
                        if (running.get()) {
                            android.util.Log.e("SimpleProxy", "Error accepting client connection", e)
                            e.printStackTrace()
                        }
                    }
                }
            } catch (e: IOException) {
                android.util.Log.e("SimpleProxy", "Error starting proxy server", e)
                e.printStackTrace()
            }
        }
    }

    private fun stopProxy() {
        running.set(false)
        try {
            serverSocket?.close()
        } catch (e: IOException) {
            e.printStackTrace()
        }
    }

    private suspend fun handleConnection(clientSocket: Socket) = withContext(Dispatchers.IO) {
        try {
            clientSocket.use { client ->
                // Read the request
                val request = client.getInputStream().bufferedReader().readLine() ?: return@use
                android.util.Log.d("SimpleProxy", "Received request: $request")
                
                // Parse the request
                val parts = request.split(" ")
                if (parts.size < 3) {
                    android.util.Log.e("SimpleProxy", "Invalid request format: $request")
                    sendErrorResponse(client, "Invalid request format")
                    return@use
                }

                when (parts[0].uppercase()) {
                    "CONNECT" -> {
                        android.util.Log.d("SimpleProxy", "Handling HTTPS CONNECT for ${parts[1]}")
                        handleHttpsConnect(client, parts[1])
                    }
                    "GET" -> {
                        android.util.Log.d("SimpleProxy", "Handling HTTP GET for ${parts[1]}")
                        handleHttpGet(client, parts[1])
                    }
                    else -> {
                        android.util.Log.e("SimpleProxy", "Unsupported method: ${parts[0]}")
                        sendErrorResponse(client, "Only GET and CONNECT methods are supported")
                    }
                }
            }
        } catch (e: IOException) {
            android.util.Log.e("SimpleProxy", "Error handling connection", e)
            e.printStackTrace()
        }
    }

    private suspend fun handleHttpsConnect(client: Socket, target: String) = withContext(Dispatchers.IO) {
        try {
            // Parse host and port
            val (host, portStr) = target.split(":")
            val port = portStr.toInt()

            // Connect to target server
            val targetSocket = Socket(host, port)

            // Send 200 Connection established
            val response = "HTTP/1.1 200 Connection Established\r\n\r\n"
            client.getOutputStream().write(response.toByteArray())

            // Start bidirectional tunneling
            scope.launch {
                try {
                    val buffer = ByteArray(8192)
                    while (running.get()) {
                        val bytesRead = targetSocket.getInputStream().read(buffer)
                        if (bytesRead == -1) break
                        client.getOutputStream().write(buffer, 0, bytesRead)
                    }
                } catch (e: IOException) {
                    // Connection closed
                }
            }

            try {
                val buffer = ByteArray(8192)
                while (running.get()) {
                    val bytesRead = client.getInputStream().read(buffer)
                    if (bytesRead == -1) break
                    targetSocket.getOutputStream().write(buffer, 0, bytesRead)
                }
            } catch (e: IOException) {
                // Connection closed
            }

            targetSocket.close()
        } catch (e: Exception) {
            sendErrorResponse(client, "HTTPS tunnel failed: ${e.message}")
        }
    }

    private suspend fun handleHttpGet(client: Socket, targetUrl: String) = withContext(Dispatchers.IO) {
        try {
            val url = java.net.URL(targetUrl)
            val connection = url.openConnection() as java.net.HttpURLConnection
            connection.requestMethod = "GET"
                    
            // Copy response headers
            val responseHeaders = StringBuilder()
            responseHeaders.append("HTTP/1.1 ${connection.responseCode}\r\n")
            connection.headerFields.forEach { (key, values) ->
                if (key != null) {
                    values.forEach { value ->
                        responseHeaders.append("$key: $value\r\n")
                    }
                }
            }
            responseHeaders.append("\r\n")
            
            // Send headers
            client.getOutputStream().write(responseHeaders.toString().toByteArray())
            
            // Copy response body
            connection.inputStream.use { input ->
                client.getOutputStream().use { output ->
                    input.copyTo(output)
                }
            }
        } catch (e: Exception) {
            sendErrorResponse(client, e.message ?: "Unknown error")
        }
    }

    private fun sendErrorResponse(client: Socket, message: String) {
        val response = """
            HTTP/1.1 500 Internal Server Error
            Content-Type: text/plain
            Content-Length: ${message.length}

            $message
        """.trimIndent()
        
        client.getOutputStream().write(response.toByteArray())
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            NOTIFICATION_CHANNEL_ID,
            "Simple Proxy Service",
            NotificationManager.IMPORTANCE_LOW
        ).apply {
            description = "Running proxy service"
        }

        val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        notificationManager.createNotificationChannel(channel)
    }

    private fun createNotification(): Notification {
        return NotificationCompat.Builder(this, NOTIFICATION_CHANNEL_ID)
            .setContentTitle("Simple Proxy")
            .setContentText("Proxy service is running on port $DEFAULT_PORT")
            .setSmallIcon(android.R.drawable.ic_menu_share)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }
}

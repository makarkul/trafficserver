package org.apache.trafficserver.test

import android.Manifest
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.net.VpnService
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import java.net.Socket
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Dns
import org.apache.trafficserver.test.databinding.ActivityMainBinding
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Proxy
import java.util.concurrent.TimeUnit
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager
import java.security.cert.X509Certificate
import java.io.IOException

class MainActivity : AppCompatActivity() {
    private var vpnService: TrafficServerVpnService? = null

    private val vpnServiceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            val binder = service as? TrafficServerVpnService.LocalBinder
            vpnService = binder?.service
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            vpnService = null
        }
    }
    companion object {
        private const val TAG = "ProxyTest"
    }

    private lateinit var binding: ActivityMainBinding
    private var proxyServiceIntent: Intent? = null
    private var vpnServiceIntent: Intent? = null
    private val VPN_REQUEST_CODE = 1

    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { isGranted: Boolean ->
        if (isGranted) {
            // Permission granted, start VPN
            val vpnIntent = VpnService.prepare(this)
            if (vpnIntent != null) {
                startActivityForResult(vpnIntent, VPN_REQUEST_CODE)
            } else {
                onVpnPermissionGranted()
            }
        } else {
            binding.textResult.text = "Notification permission denied"
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        
        // Create service intents
        proxyServiceIntent = Intent(this, TrafficServerProxyService::class.java)
        vpnServiceIntent = Intent(this, TrafficServerVpnService::class.java)
        
        setupUI()

        // Request notification permission for Android 13+
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            when {
                ContextCompat.checkSelfPermission(
                    this,
                    Manifest.permission.POST_NOTIFICATIONS
                ) == PackageManager.PERMISSION_GRANTED -> {
                    // Permission already granted, start VPN
                    val vpnIntent = VpnService.prepare(this)
                    if (vpnIntent != null) {
                        startActivityForResult(vpnIntent, VPN_REQUEST_CODE)
                    } else {
                        onVpnPermissionGranted()
                    }
                }
                else -> {
                    // Request notification permission
                    requestPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
                }
            }
        } else {
            // Below Android 13, no runtime permission needed
            val vpnIntent = VpnService.prepare(this)
            if (vpnIntent != null) {
                startActivityForResult(vpnIntent, VPN_REQUEST_CODE)
            } else {
                onVpnPermissionGranted()
            }
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == VPN_REQUEST_CODE && resultCode == RESULT_OK) {
            onVpnPermissionGranted()
        } else {
            binding.textResult.text = "VPN permission denied"
        }
    }

    private fun onVpnPermissionGranted() {
        lifecycleScope.launch {
            try {
                binding.textResult.text = "Starting services..."

                // Start proxy service first
                android.util.Log.i(TAG, "Starting proxy service")
                startForegroundService(proxyServiceIntent!!)
                delay(1000) // Wait for proxy to initialize

                // Bind to VPN service
                bindService(
                    Intent(this@MainActivity, TrafficServerVpnService::class.java),
                    vpnServiceConnection,
                    Context.BIND_AUTO_CREATE
                )

                // Make sure proxy is running before starting VPN
                if (!checkProxyRunning()) {
                    binding.textResult.text = "Failed to start proxy service"
                    android.util.Log.e(TAG, "Failed to verify proxy service")
                    return@launch
                }

                // Start VPN service after proxy is confirmed running
                vpnServiceIntent?.let { intent ->
                    android.util.Log.i(TAG, "Starting VPN service")
                    startForegroundService(intent)
                    delay(2000) // Give VPN more time to initialize
                }

                if (checkProxyRunning()) {
                    binding.buttonToggleProxy.text = "Stop Services"
                    binding.textResult.text = "VPN and Proxy services are running"
                    android.util.Log.i(TAG, "Both services started successfully")
                } else {
                    binding.buttonToggleProxy.text = "Start Services"
                    binding.textResult.text = "Failed to start services"
                    android.util.Log.e(TAG, "Failed to verify proxy service")
                }
            } catch (e: Exception) {
                android.util.Log.e(TAG, "Failed to start services", e)
                binding.textResult.text = "Failed to start services: ${e.message}"
            }
        }
    }

    private fun setupUI() {
        binding.buttonDirectRequest.setOnClickListener {
            makeRequest(useProxy = false)
        }
        
        binding.buttonProxyRequest.setOnClickListener {
            makeRequest(useProxy = true)
        }

        binding.buttonToggleProxy.setOnClickListener {
            lifecycleScope.launch {
                if (checkProxyRunning()) {
                    android.util.Log.i(TAG, "Stopping services")
                    binding.textResult.text = "Stopping services..."

                    // Stop both services
                    proxyServiceIntent?.let { intent ->
                        stopService(intent)
                    }
                    vpnServiceIntent?.let { intent ->
                        stopService(intent)
                    }
                    binding.buttonToggleProxy.text = "Start Services"
                    binding.textResult.text = "Services stopped"
                    android.util.Log.i(TAG, "Services stopped")
                } else {
                    android.util.Log.i(TAG, "Starting services")
                    binding.textResult.text = "Starting services..."

                    // Start proxy service first
                    proxyServiceIntent?.let { intent ->
                        android.util.Log.i(TAG, "Starting proxy service")
                        startForegroundService(intent)
                    }
                    // Wait for proxy to start
                    withContext(Dispatchers.IO) {
                        delay(1000)
                    }

                    // Make sure proxy is running before starting VPN
                    if (!checkProxyRunning()) {
                        binding.textResult.text = "Failed to start proxy service"
                        android.util.Log.e(TAG, "Failed to verify proxy service")
                        return@launch
                    }

                    // Start VPN service after proxy is confirmed running
                    vpnServiceIntent?.let { intent ->
                        android.util.Log.i(TAG, "Starting VPN service")
                        startForegroundService(intent)
                    }
                    // Wait for VPN to start
                    withContext(Dispatchers.IO) {
                        delay(2000)
                    }
                    if (checkProxyRunning()) {
                        binding.buttonToggleProxy.text = "Stop Services"
                        binding.textResult.text = "VPN and Proxy services started"
                        android.util.Log.i(TAG, "Both services started successfully")
                    } else {
                        binding.textResult.text = "Failed to start services"
                        android.util.Log.e(TAG, "Failed to verify proxy service")
                    }
                }
            }
        }
        
        // Add test URLs
        val urls = arrayOf(
            "http://example.com",
            "http://neverssl.com"
        )
        binding.spinnerUrls.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, urls)
    }
    
    private suspend fun checkProxyRunning(): Boolean = withContext(Dispatchers.IO) {
        android.util.Log.d(TAG, "Checking if proxy is running...")
        try {
            // Try to make a test request through the proxy
            val testClient = OkHttpClient.Builder()
                .proxy(Proxy(Proxy.Type.HTTP, InetSocketAddress("127.0.0.1", 8888)))
                .connectTimeout(5, TimeUnit.SECONDS)
                .readTimeout(5, TimeUnit.SECONDS)
                .build()

            val testRequest = Request.Builder()
                .url("http://example.com")
                .build()

            android.util.Log.d(TAG, "Making test request through proxy...")
            testClient.newCall(testRequest).execute().use { response ->
                android.util.Log.d(TAG, "Got response: ${response.code}")
                response.isSuccessful
            }
        } catch (e: Exception) {
            android.util.Log.e(TAG, "Error checking proxy", e)
            android.util.Log.e(TAG, "Stack trace: ${e.stackTraceToString()}")
            false
        }
    }

    private fun makeRequest(useProxy: Boolean) {
        android.util.Log.d(TAG, "Making ${if (useProxy) "proxy" else "direct"} request...")
        binding.buttonDirectRequest.isEnabled = false
        binding.buttonProxyRequest.isEnabled = false
        binding.textResult.text = "Preparing request..."

        lifecycleScope.launch(Dispatchers.Main) {
            try {
                if (useProxy) {
                    binding.textResult.text = "Checking proxy status..."
                    // Ensure proxy is running
                    if (!checkProxyRunning()) {
                        binding.textResult.text = "Starting proxy service..."
                        startForegroundService(proxyServiceIntent!!)

                        // Wait for proxy to start with multiple retries
                        var proxyStarted = false
                        repeat(3) { attempt ->
                            try {
                                withContext(Dispatchers.IO) {
                                    // Gradually increase wait time between attempts
                                    delay(1000L * (attempt + 1))
                                    val socket = Socket()
                                    socket.connect(InetSocketAddress("127.0.0.1", 8888), 1000)
                                    socket.close()
                                    proxyStarted = true
                                }
                                if (proxyStarted) return@repeat
                            } catch (e: Exception) {
                                android.util.Log.w(TAG, "Proxy start attempt ${attempt + 1} failed", e)
                                withContext(Dispatchers.Main) {
                                    binding.textResult.text = "Waiting for proxy (attempt ${attempt + 1}/3)..."
                                }
                                if (attempt == 2) {
                                    withContext(Dispatchers.Main) {
                                        binding.textResult.text = "Error: Could not connect to proxy after 3 attempts"
                                        binding.buttonDirectRequest.isEnabled = true
                                        binding.buttonProxyRequest.isEnabled = true
                                    }
                                    return@launch
                                }
                            }
                        }
                    }
                }

                binding.textResult.text = "Loading..."
                binding.buttonDirectRequest.isEnabled = false
                binding.buttonProxyRequest.isEnabled = false
                
                binding.textResult.text = "Sending request..."
                val startTime = System.currentTimeMillis()
                val result = withContext(Dispatchers.IO) {
                    android.util.Log.d(TAG, "Creating request...")
                    var retryCount = 0
                    var lastError: Exception? = null
                    
                    while (retryCount < 2) {
                        try {
                            val client = createHttpClient(useProxy)
                            val url = binding.spinnerUrls.selectedItem.toString()
                            android.util.Log.d(TAG, "URL: $url")
                            val request = Request.Builder()
                                .url(url)
                                .header("User-Agent", "TrafficServer-Test/1.0")
                                .header("Accept", "*/*")
                                .build()
                            android.util.Log.d(TAG, "Request headers: ${request.headers}")
                            android.util.Log.d(TAG, "Executing request (attempt ${retryCount + 1})...")
                            
                            return@withContext client.newCall(request).execute().use { response ->
                                buildString {
                                    append("URL: $url\n")
                                    append("Connection: ${if (useProxy) "\uD83D\uDFE2 Through TrafficServer proxy at ${client.proxy}" else "\uD83D\uDD34 Direct"}\n")
                                    append("Time: ${System.currentTimeMillis() - startTime}ms\n\n")
                                    append("Status: ${response.code}\n")
                                    append("Protocol: ${response.protocol}\n\n")
                                    
                                    if (useProxy) {
                                        append("\nProxy Details:\n")
                                        append("• Protocol: ${if (url.startsWith("https")) "HTTPS (CONNECT tunnel)" else "HTTP"}\n")
                                        append("• Proxy Headers:\n")
                                        val proxyHeaders = listOf(
                                            "Via",
                                            "Proxy-Connection",
                                            "Proxy-Agent",
                                            "X-Proxy-Through"
                                        )
                                        var foundProxyHeaders = false
                                        proxyHeaders.forEach { header ->
                                            response.header(header)?.let { value ->
                                                append("  - $header: $value\n")
                                                foundProxyHeaders = true
                                            }
                                        }
                                        if (!foundProxyHeaders) {
                                            if (url.startsWith("https")) {
                                                append("  (No proxy headers visible in HTTPS response - this is normal)\n")
                                            } else {
                                                append("  (No proxy headers found)\n")
                                            }
                                        }
                                    }
                                    
                                    append("\nAll Headers:\n")
                                    response.headers.forEach { (name, value) ->
                                        append("$name: $value\n")
                                    }
                                    append("\nBody:\n")
                                    append(response.body?.string()?.take(500) ?: "Empty body")
                                }
                            }
                        } catch (e: Exception) {
                            lastError = e
                            android.util.Log.e(TAG, "Request attempt ${retryCount + 1} failed", e)
                            if (useProxy) {
                                withContext(Dispatchers.Main) {
                                    binding.textResult.text = "Retrying request (attempt ${retryCount + 2})..."
                                }
                                // Wait before retry
                                delay(1000L * (retryCount + 1))
                                retryCount++
                                if (retryCount < 2) continue
                            }
                            throw e
                        }
                    }
                    throw lastError ?: IOException("Request failed after multiple attempts")
                }
                
                binding.textResult.text = result
            } catch (e: Exception) {
                android.util.Log.e(TAG, "Request failed", e)
                android.util.Log.e(TAG, "Stack trace: ${e.stackTraceToString()}")
                binding.textResult.text = "Error: ${e.message}"
                if (!isFinishing) {
                    Toast.makeText(this@MainActivity, "Request failed - ${e.message}", Toast.LENGTH_LONG).show()
                }
            } finally {
                binding.buttonDirectRequest.isEnabled = true
                binding.buttonProxyRequest.isEnabled = true
            }
        }
    }

    private suspend fun createHttpClient(useProxy: Boolean): OkHttpClient {
        android.util.Log.d(TAG, "Creating HTTP client (useProxy=$useProxy)")
        return OkHttpClient.Builder().apply {
            if (useProxy) {
                android.util.Log.d(TAG, "Creating client with proxy...")
                // First check if proxy is running
                if (!checkProxyRunning()) {
                    throw IOException("Proxy is not running. Please start the proxy first.")
                }
                proxy(Proxy(
                    Proxy.Type.HTTP,
                    InetSocketAddress("127.0.0.1", 8888)
                ))
            } else {
                // For direct requests, use NO_PROXY but let the VPN handle routing
                proxy(Proxy.NO_PROXY)
                
                // Use custom DNS resolver that goes through VPN
                dns(object : Dns {
                    override fun lookup(hostname: String): List<InetAddress> {
                        return try {
                            android.util.Log.d(TAG, "Looking up DNS for $hostname")
                            InetAddress.getAllByName(hostname).toList()
                        } catch (e: Exception) {
                            android.util.Log.e(TAG, "DNS lookup failed for $hostname", e)
                            throw e
                        }
                    }
                })
                
                // Use custom socket factory that goes through VPN
                socketFactory(object : javax.net.SocketFactory() {
                    override fun createSocket(): Socket {
                        return Socket().apply {
                            // Don't protect the socket - let VPN handle it
                        }
                    }
                    
                    override fun createSocket(host: String, port: Int): Socket {
                        return createSocket().apply {
                            connect(InetSocketAddress(host, port))
                        }
                    }
                    
                    override fun createSocket(host: String, port: Int, localHost: InetAddress, localPort: Int): Socket {
                        return createSocket().apply {
                            bind(InetSocketAddress(localHost, localPort))
                            connect(InetSocketAddress(host, port))
                        }
                    }
                    
                    override fun createSocket(host: InetAddress, port: Int): Socket {
                        return createSocket().apply {
                            connect(InetSocketAddress(host, port))
                        }
                    }
                    
                    override fun createSocket(address: InetAddress, port: Int, localAddress: InetAddress, localPort: Int): Socket {
                        return createSocket().apply {
                            bind(InetSocketAddress(localAddress, localPort))
                            connect(InetSocketAddress(address, port))
                        }
                    }
                })
            }

            // Add logging interceptor
            addInterceptor { chain ->
                val request = chain.request()
                android.util.Log.d(TAG, "${request.method} ${request.url}")
                android.util.Log.d(TAG, "Request headers: ${request.headers}")
                try {
                    chain.proceed(request).also { response ->
                        android.util.Log.d(TAG, "Response: ${response.code} ${response.message}")
                        android.util.Log.d(TAG, "Response headers: ${response.headers}")
                    }
                } catch (e: Exception) {
                    android.util.Log.e(TAG, "Request failed: ${e.message}")
                    throw e
                }
            }

            // Increase timeouts for proxy connections
            if (useProxy) {
                connectTimeout(30, TimeUnit.SECONDS)
                readTimeout(30, TimeUnit.SECONDS)
                writeTimeout(30, TimeUnit.SECONDS)
            } else {
                connectTimeout(10, TimeUnit.SECONDS)
                readTimeout(10, TimeUnit.SECONDS)
                writeTimeout(10, TimeUnit.SECONDS)
            }

            // Retry on connection failure
            retryOnConnectionFailure(true)
        }.build().also { client ->
            android.util.Log.d(TAG, "Created client with proxy: ${client.proxy}")
        }
    }
    
}

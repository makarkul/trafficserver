package org.apache.trafficserver.test

import android.content.Intent
import android.os.Bundle
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import java.net.Socket
import kotlinx.coroutines.Dispatchers
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

class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        
        setupUI()
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
                    // Stop proxy
                    Intent(this@MainActivity, TrafficServerProxyService::class.java).also { intent ->
                        stopService(intent)
                    }
                    binding.buttonToggleProxy.text = "Start Proxy"
                    binding.textResult.text = "Proxy stopped"
                } else {
                    // Start proxy
                    Intent(this@MainActivity, TrafficServerProxyService::class.java).also { intent ->
                        startForegroundService(intent)
                    }
                    // Wait for proxy to start
                    withContext(Dispatchers.IO) {
                        Thread.sleep(1000)
                    }
                    if (checkProxyRunning()) {
                        binding.buttonToggleProxy.text = "Stop Proxy"
                        binding.textResult.text = "Proxy started on port 8888"
                    } else {
                        binding.textResult.text = "Failed to start proxy"
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
        android.util.Log.d("ProxyTest", "Checking if proxy is running...")
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

            android.util.Log.d("ProxyTest", "Making test request through proxy...")
            testClient.newCall(testRequest).execute().use { response ->
                android.util.Log.d("ProxyTest", "Got response: ${response.code}")
                response.isSuccessful
            }
        } catch (e: Exception) {
            android.util.Log.e("ProxyTest", "Error checking proxy", e)
            android.util.Log.e("ProxyTest", "Stack trace: ${e.stackTraceToString()}")
            false
        }
    }

    companion object {
        private const val TAG = "ProxyTest"
    }

    private fun makeRequest(useProxy: Boolean) {
        android.util.Log.d(TAG, "Making ${if (useProxy) "proxy" else "direct"} request...")
        binding.buttonDirectRequest.isEnabled = false
        binding.buttonProxyRequest.isEnabled = false
        lifecycleScope.launch {
            try {
                if (useProxy) {
                    // Start TrafficServer proxy if not running
                    Intent(this@MainActivity, TrafficServerProxyService::class.java).also { intent ->
                        startForegroundService(intent)
                    }

                    // Wait for proxy to start and check if it's running
                    try {
                        withContext(Dispatchers.IO) {
                            // Wait a bit for the proxy to start
                            Thread.sleep(1000)

                            // Check if proxy is running
                            val socket = Socket()
                            try {
                                socket.connect(InetSocketAddress("127.0.0.1", 8888), 1000)
                                socket.close()
                            } catch (e: Exception) {
                                throw e
                            }
                        }
                    } catch (e: Exception) {
                        android.util.Log.e(TAG, "Proxy not running", e)
                        binding.textResult.text = "Error: Proxy not running"
                        binding.buttonDirectRequest.isEnabled = true
                        binding.buttonProxyRequest.isEnabled = true
                        return@launch
                    }
                }

                binding.textResult.text = "Loading..."
                binding.buttonDirectRequest.isEnabled = false
                binding.buttonProxyRequest.isEnabled = false
                
                val startTime = System.currentTimeMillis()
                val result = withContext(Dispatchers.IO) {
                    android.util.Log.d(TAG, "Creating request...")
                val client = createHttpClient(useProxy)
                val url = binding.spinnerUrls.selectedItem.toString()
                android.util.Log.d(TAG, "URL: $url")
                val request = Request.Builder()
                    .url(url)
                    .header("User-Agent", "TrafficServer-Test/1.0")
                    .header("Accept", "*/*")
                    .build()
                android.util.Log.d(TAG, "Request headers: ${request.headers}")
                android.util.Log.d(TAG, "Executing request...")
                        
                    client.newCall(request).execute().use { response ->
                        val endTime = System.currentTimeMillis()
                        buildString {
                            append("URL: $url\n")
                            append("Connection: ${if (useProxy) "\uD83D\uDFE2 Through TrafficServer proxy at ${client.proxy}" else "\uD83D\uDD34 Direct"}\n")
                            append("Time: ${endTime - startTime}ms\n\n")
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
                }
                
                binding.textResult.text = result
            } catch (e: Exception) {
                android.util.Log.e("ProxyTest", "Request failed", e)
                android.util.Log.e("ProxyTest", "Stack trace: ${e.stackTraceToString()}")
                binding.textResult.text = "Error: ${e.message}"
                Toast.makeText(this@MainActivity, "Request failed", Toast.LENGTH_SHORT).show()
            } finally {
                binding.buttonDirectRequest.isEnabled = true
                binding.buttonProxyRequest.isEnabled = true
            }
        }
    }
    
    private fun createHttpClient(useProxy: Boolean): OkHttpClient {
        android.util.Log.d(TAG, "Creating HTTP client (useProxy=$useProxy)")
        return OkHttpClient.Builder().apply {
            if (useProxy) {
                android.util.Log.d(TAG, "Creating client with proxy...")
                proxy(Proxy(
                    Proxy.Type.HTTP,
                    InetSocketAddress("127.0.0.1", 8888)
                ))
            } else {
                proxy(Proxy.NO_PROXY)
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

            connectTimeout(10, TimeUnit.SECONDS)
            readTimeout(10, TimeUnit.SECONDS)
            writeTimeout(10, TimeUnit.SECONDS)
        }.build().also { client ->
            android.util.Log.d(TAG, "Created client with proxy: ${client.proxy}")
        }
    }
}

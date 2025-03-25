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
import org.apache.trafficserver.test.databinding.ActivityMainBinding
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
        
        // Add test URLs
        binding.spinnerUrls.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, listOf(
            "https://example.com",
            "http://example.com",
            "https://api.github.com/zen",
            "http://info.cern.ch"
        ))
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

    private fun makeRequest(useProxy: Boolean) {
        lifecycleScope.launch {
            try {
                if (useProxy) {
                    // Start TrafficServer proxy if not running
                    Intent(this@MainActivity, TrafficServerProxyService::class.java).also { intent ->
                        startForegroundService(intent)
                        // Wait a bit for the proxy to start
                        Thread.sleep(1000)
                    }
                }

                binding.textResult.text = "Loading..."
                binding.buttonDirectRequest.isEnabled = false
                binding.buttonProxyRequest.isEnabled = false
                
                val startTime = System.currentTimeMillis()
                val result = withContext(Dispatchers.IO) {
                    val client = createHttpClient(useProxy)
                    val url = binding.spinnerUrls.selectedItem.toString()
                    val request = Request.Builder()
                        .url(url)
                        .build()
                        
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
                binding.textResult.text = "Error: ${e.message}"
                Toast.makeText(this@MainActivity, "Request failed", Toast.LENGTH_SHORT).show()
            } finally {
                binding.buttonDirectRequest.isEnabled = true
                binding.buttonProxyRequest.isEnabled = true
            }
        }
    }
    
    private fun createHttpClient(useProxy: Boolean): OkHttpClient {
        return OkHttpClient.Builder().apply {
            if (useProxy) {
                android.util.Log.d("ProxyTest", "Creating client with proxy...")
                proxy(Proxy(
                    Proxy.Type.HTTP,  // Use HTTP proxy type for both HTTP and HTTPS
                    InetSocketAddress("127.0.0.1", 8888)
                ))
                // Trust all certificates when going through proxy
                val trustAllCerts = arrayOf<TrustManager>(object : X509TrustManager {
                    override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
                    override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
                    override fun getAcceptedIssuers(): Array<X509Certificate> = arrayOf()
                })
                val sslContext = SSLContext.getInstance("SSL")
                sslContext.init(null, trustAllCerts, java.security.SecureRandom())
                sslSocketFactory(sslContext.socketFactory, trustAllCerts[0] as X509TrustManager)
                hostnameVerifier { _, _ -> true }
            }
            connectTimeout(10, TimeUnit.SECONDS)
            readTimeout(10, TimeUnit.SECONDS)
            writeTimeout(10, TimeUnit.SECONDS)
        }.build().also { client ->
            if (useProxy) {
                android.util.Log.d("ProxyTest", "Created client with proxy: ${client.proxy}")
            }
        }
    }
}

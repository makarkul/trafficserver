package org.apache.trafficserver.android

import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.apache.trafficserver.android.databinding.ActivityMainBinding
import java.net.HttpURLConnection
import java.net.InetSocketAddress
import java.net.Proxy

class MainActivity : AppCompatActivity() {
    private lateinit var binding: ActivityMainBinding
    private var proxyRunning = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        setupUI()
    }

    private fun setupUI() {
        binding.buttonStartProxy.setOnClickListener {
            startProxy()
        }

        binding.buttonStopProxy.setOnClickListener {
            stopProxy()
        }

        binding.buttonTestProxy.setOnClickListener {
            testProxy()
        }

        updateProxyStatus(false)
    }

    private fun startProxy() {
        Intent(this, SimpleProxyService::class.java).also { intent ->
            startForegroundService(intent)
            updateProxyStatus(true)
            Toast.makeText(this, "Proxy service started", Toast.LENGTH_SHORT).show()
        }
    }

    private fun stopProxy() {
        Intent(this, SimpleProxyService::class.java).also { intent ->
            stopService(intent)
            updateProxyStatus(false)
            Toast.makeText(this, "Proxy service stopped", Toast.LENGTH_SHORT).show()
        }
    }

    private fun updateProxyStatus(running: Boolean) {
        proxyRunning = running
        binding.textStatus.text = if (running) "Proxy is running" else "Proxy is stopped"
        binding.buttonStartProxy.isEnabled = !running
        binding.buttonStopProxy.isEnabled = running
        binding.buttonTestProxy.isEnabled = running
    }

    private fun testProxy() {
        lifecycleScope.launch {
            try {
                binding.buttonTestProxy.isEnabled = false
                binding.textTestResult.text = "Testing..."

                val result = withContext(Dispatchers.IO) {
                    testProxyConnection()
                }

                binding.textTestResult.text = "Test result: $result"
            } catch (e: Exception) {
                binding.textTestResult.text = "Test failed: ${e.message}"
            } finally {
                binding.buttonTestProxy.isEnabled = proxyRunning
            }
        }
    }

    private fun testProxyConnection(): String {
        return try {
            val proxy = Proxy(
                Proxy.Type.HTTP,
                InetSocketAddress("127.0.0.1", 8080)
            )
            
            val url = java.net.URL("http://example.com")
            val connection = url.openConnection(proxy) as HttpURLConnection
            connection.connectTimeout = 5000
            connection.readTimeout = 5000
            
            val responseCode = connection.responseCode
            if (responseCode == 200) {
                "Success! Response code: $responseCode"
            } else {
                "Unexpected response: $responseCode"
            }
        } catch (e: Exception) {
            "Failed: ${e.message}"
        }
    }
}

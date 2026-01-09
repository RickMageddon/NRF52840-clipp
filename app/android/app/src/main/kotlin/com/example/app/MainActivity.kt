package com.example.app

import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.provider.Settings
import android.os.Build

class MainActivity : FlutterActivity() {
    private val CHANNEL = "com.example.app/events"
    private var methodChannel: MethodChannel? = null
    
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        
        methodChannel = MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL)
        
        // Set up a static reference so BroadcastReceivers and Services can send events
        EventBus.methodChannel = methodChannel
        
        // Set up method call handler for permissions and settings
        methodChannel?.setMethodCallHandler { call, result ->
            when (call.method) {
                "isNotificationListenerEnabled" -> {
                    val isEnabled = isNotificationListenerEnabled()
                    result.success(isEnabled)
                }
                "openNotificationSettings" -> {
                    openNotificationSettings()
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        }
    }
    
    private fun isNotificationListenerEnabled(): Boolean {
        val notificationManager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            val enabledListeners = Settings.Secure.getString(
                contentResolver,
                "enabled_notification_listeners"
            ) ?: return false
            val packageName = packageName
            return enabledListeners.contains(packageName)
        }
        return false
    }
    
    private fun openNotificationSettings() {
        val intent = Intent()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP_MR1) {
            intent.action = Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS
        } else {
            intent.action = "android.settings.ACTION_NOTIFICATION_LISTENER_SETTINGS"
        }
        startActivity(intent)
    }
    
    companion object {
        const val CHANNEL = "com.example.app/events"
    }
}

// Event bus to allow BroadcastReceivers and Services to send events to Flutter
object EventBus {
    var methodChannel: MethodChannel? = null
    
    fun sendCallEvent(phoneNumber: String, duration: Long) {
        methodChannel?.invokeMethod("onCall", mapOf(
            "phoneNumber" to phoneNumber,
            "duration" to duration
        ))
    }
    
    fun sendNotificationEvent(appName: String, title: String, message: String) {
        methodChannel?.invokeMethod("onNotification", mapOf(
            "appName" to appName,
            "title" to title,
            "message" to message
        ))
    }
}

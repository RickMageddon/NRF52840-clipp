package com.example.app

import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

class NotificationListenerService : NotificationListenerService() {
    
    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        super.onNotificationPosted(sbn)
        
        if (sbn != null) {
            val packageName = sbn.packageName
            val notification = sbn.notification
            val extras = notification.extras
            
            // Extract notification details
            val title = extras.getCharSequence("android.title")?.toString() ?: ""
            val message = extras.getCharSequence("android.text")?.toString() ?: ""
            
            Log.d("NotificationListener", "Notification from: $packageName, Title: $title, Message: $message")
            
            // Send to Flutter
            EventBus.sendNotificationEvent(packageName, title, message)
        }
    }
    
    override fun onNotificationRemoved(sbn: StatusBarNotification?) {
        super.onNotificationRemoved(sbn)
        // Optional: track when notifications are dismissed
    }
}

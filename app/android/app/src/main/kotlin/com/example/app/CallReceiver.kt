package com.example.app

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.telephony.TelephonyManager
import android.util.Log

class CallReceiver : BroadcastReceiver() {
    companion object {
        private var callStartTime = 0L
        private var callNumber = ""
    }
    
    override fun onReceive(context: Context?, intent: Intent?) {
        if (intent?.action == TelephonyManager.ACTION_PHONE_STATE_CHANGED) {
            val state = intent.getStringExtra(TelephonyManager.EXTRA_STATE)
            val phoneNumber = intent.getStringExtra(TelephonyManager.EXTRA_INCOMING_NUMBER) ?: "Unknown"
            
            when (state) {
                TelephonyManager.EXTRA_STATE_RINGING -> {
                    // Incoming call started ringing
                    callNumber = phoneNumber
                    callStartTime = System.currentTimeMillis()
                    Log.d("CallReceiver", "Incoming call from: $phoneNumber")
                }
                
                TelephonyManager.EXTRA_STATE_OFFHOOK -> {
                    // Call answered
                    callStartTime = System.currentTimeMillis()
                    Log.d("CallReceiver", "Call answered: $phoneNumber")
                }
                
                TelephonyManager.EXTRA_STATE_IDLE -> {
                    // Call ended
                    if (callStartTime > 0) {
                        val duration = (System.currentTimeMillis() - callStartTime) / 1000
                        Log.d("CallReceiver", "Call ended. Duration: $duration seconds")
                        EventBus.sendCallEvent(callNumber, duration)
                        callStartTime = 0L
                        callNumber = ""
                    }
                }
            }
        }
    }
}

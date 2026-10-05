package com.vikas.clawdhandy

import android.app.AlarmManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Shows note reminders, and puts them back after the phone restarts. */
class ReminderReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED) {
            reschedule(context)
            return
        }
        val id = intent.getLongExtra(EXTRA_ID, System.currentTimeMillis())
        Store.updateNotes(context) { note -> if (note.optLong("id") == id) note.put("remindAt", 0) else note }
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel(CHANNEL_ID, "Clawd reminders", NotificationManager.IMPORTANCE_HIGH))
        val open = PendingIntent.getActivity(context, 0, Intent(context, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val notification = Notification.Builder(context, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.ic_popup_reminder)
            .setContentTitle("Clawd reminder")
            .setContentText(intent.getStringExtra(EXTRA_NOTE).orEmpty())
            .setContentIntent(open)
            .setAutoCancel(true)
            .build()
        manager.notify((id % Int.MAX_VALUE).toInt(), notification)
    }

    companion object {
        private const val CHANNEL_ID = "clawd_notes"
        private const val EXTRA_ID = "note_id"
        private const val EXTRA_NOTE = "note_text"

        fun schedule(context: Context, id: Long, text: String, atMillis: Long) {
            context.getSystemService(AlarmManager::class.java)
                .setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, atMillis, pendingIntent(context, id, text))
        }

        fun cancel(context: Context, id: Long) {
            context.getSystemService(AlarmManager::class.java).cancel(pendingIntent(context, id, ""))
        }

        private fun reschedule(context: Context) {
            val notes = Store.notes(context)
            val now = System.currentTimeMillis()
            for (i in 0 until notes.length()) {
                val note = notes.optJSONObject(i) ?: continue
                val at = note.optLong("remindAt")
                if (at > now) schedule(context, note.optLong("id"), note.optString("text"), at)
            }
        }

        private fun pendingIntent(context: Context, id: Long, text: String): PendingIntent {
            val intent = Intent(context, ReminderReceiver::class.java)
                .setAction("com.vikas.clawdhandy.REMINDER.$id")
                .putExtra(EXTRA_ID, id)
                .putExtra(EXTRA_NOTE, text)
            return PendingIntent.getBroadcast(
                context, (id % Int.MAX_VALUE).toInt(), intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
        }
    }
}

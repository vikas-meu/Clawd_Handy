package com.vikas.clawdhandy

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.media.AudioManager
import android.net.Uri
import android.provider.MediaStore
import android.provider.Settings
import android.view.KeyEvent

/** Things Claude can do on the phone when you ask the handheld (or the app). */
object PhoneActions {
    private const val CHANNEL = "clawd_actions"

    /** True while MainActivity is on screen: then apps can be opened directly. */
    @Volatile var appInForeground = false

    fun media(context: Context, action: String): String {
        val audio = context.getSystemService(AudioManager::class.java)
        fun key(code: Int) {
            audio.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, code))
            audio.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, code))
        }
        return when (action) {
            "play_pause" -> { key(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE); "Toggled play/pause" }
            "next" -> { key(KeyEvent.KEYCODE_MEDIA_NEXT); "Skipped to the next track" }
            "previous" -> { key(KeyEvent.KEYCODE_MEDIA_PREVIOUS); "Went back a track" }
            "volume_up" -> { audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE, 0); "Volume up" }
            "volume_down" -> { audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, 0); "Volume down" }
            "mute" -> { audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_TOGGLE_MUTE, 0); "Toggled mute" }
            else -> "Unknown media action $action"
        }
    }

    /** Opens maps, music, the dialer or a web page. Returns what happened, for Claude to relay. */
    fun open(context: Context, what: String, query: String): String {
        val q = Uri.encode(query)
        val (intent, label) = when (what) {
            "navigate" -> Intent(Intent.ACTION_VIEW, Uri.parse("google.navigation:q=$q")) to "Directions to $query"
            "map_search" -> Intent(Intent.ACTION_VIEW, Uri.parse("geo:0,0?q=$q")) to "Map search: $query"
            "play_music" -> Intent(MediaStore.INTENT_ACTION_MEDIA_PLAY_FROM_SEARCH).apply {
                putExtra(MediaStore.EXTRA_MEDIA_FOCUS, "vnd.android.cursor.item/*")
                putExtra(android.app.SearchManager.QUERY, query)
            } to "Play $query"
            "dial" -> Intent(Intent.ACTION_DIAL, Uri.parse("tel:$q")) to "Call $query"
            "open_url" -> Intent(Intent.ACTION_VIEW, Uri.parse(if (query.startsWith("http")) query else "https://$query")) to "Open $query"
            else -> Intent(Intent.ACTION_VIEW, Uri.parse("https://www.google.com/search?q=$q")) to "Search: $query"
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        // Android blocks background apps from opening screens unless they may draw over other apps
        val canOpen = appInForeground || Settings.canDrawOverlays(context)
        if (canOpen) {
            try {
                context.startActivity(intent)
                return "Opened on the phone: $label"
            } catch (_: android.content.ActivityNotFoundException) {
                if (what == "navigate") { // no Google Maps: any map app
                    intent.data = Uri.parse("geo:0,0?q=$q")
                    runCatching { context.startActivity(intent) }.onSuccess { return "Opened on the phone: $label" }
                }
            } catch (_: Exception) {
                // fall through to the notification so the user still sees it
            }
        }
        notify(context, label, intent)
        return "Sent to the phone as a notification (tap to open): $label"
    }

    fun remind(context: Context, text: String, minutes: Int): String {
        val at = System.currentTimeMillis() + minutes.coerceIn(1, 7 * 24 * 60) * 60_000L
        val id = Store.addNote(context, text, at)
        ReminderReceiver.schedule(context, id, text, at)
        return "Reminder set on the phone for $minutes min from now"
    }

    private fun notify(context: Context, title: String, intent: Intent) {
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel(CHANNEL, "Clawd actions", NotificationManager.IMPORTANCE_HIGH))
        val id = (System.currentTimeMillis() % Int.MAX_VALUE).toInt()
        val tap = PendingIntent.getActivity(context, id, intent, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val notification = Notification.Builder(context, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_menu_send)
            .setContentTitle("Clawd: $title")
            .setContentText("Tap to open")
            .setContentIntent(tap)
            .setAutoCancel(true)
            .build()
        manager.notify(id, notification)
    }
}

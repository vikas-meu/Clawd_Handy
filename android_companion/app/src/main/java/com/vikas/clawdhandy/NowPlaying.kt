package com.vikas.clawdhandy

import android.content.ComponentName
import android.content.Context
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.SystemClock
import android.provider.Settings
import android.service.notification.NotificationListenerService
import org.json.JSONObject
import java.io.ByteArrayOutputStream

/**
 * Android only shares other apps' media sessions (what's playing, album art, controls) with a
 * notification listener the user has allowed in Settings > Notification access. This one does
 * nothing else: it's just the key that unlocks [NowPlaying].
 */
class NowPlayingListener : NotificationListenerService()

/** What's playing on the phone, for the handheld's Now Playing screen. */
object NowPlaying {
    fun allowed(context: Context): Boolean =
        Settings.Secure.getString(context.contentResolver, "enabled_notification_listeners")
            ?.contains(ComponentName(context, NowPlayingListener::class.java).flattenToString()) == true

    /** The session that's playing, or else the most recent one. */
    private fun controller(context: Context): MediaController? {
        if (!allowed(context)) return null
        val sessions = runCatching {
            context.getSystemService(MediaSessionManager::class.java)
                .getActiveSessions(ComponentName(context, NowPlayingListener::class.java))
        }.getOrNull() ?: return null
        return sessions.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING } ?: sessions.firstOrNull()
    }

    private fun art(controller: MediaController): Bitmap? = controller.metadata?.let {
        it.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: it.getBitmap(MediaMetadata.METADATA_KEY_ART)
            ?: it.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON)
    }

    fun current(context: Context): JSONObject {
        if (!allowed(context)) {
            return JSONObject().put("ok", false).put("error", "Allow notification access in the Clawd Phone app")
        }
        val c = controller(context) ?: return JSONObject().put("ok", true).put("playing", false).put("title", "")
        val meta = c.metadata
        val state = c.playbackState
        val playing = state?.state == PlaybackState.STATE_PLAYING
        var position = state?.position ?: 0L
        if (playing && state != null && state.lastPositionUpdateTime > 0) {
            position += ((SystemClock.elapsedRealtime() - state.lastPositionUpdateTime) * state.playbackSpeed).toLong()
        }
        val title = meta?.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val artist = meta?.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: meta?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST) ?: ""
        val app = runCatching {
            context.packageManager.getApplicationLabel(context.packageManager.getApplicationInfo(c.packageName, 0)).toString()
        }.getOrDefault(c.packageName)
        return JSONObject()
            .put("ok", true)
            .put("playing", playing)
            .put("title", BoardText.ascii(title))
            .put("artist", BoardText.ascii(artist))
            .put("album", BoardText.ascii(meta?.getString(MediaMetadata.METADATA_KEY_ALBUM) ?: ""))
            .put("app", BoardText.ascii(app))
            .put("position_ms", position)
            .put("duration_ms", meta?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L)
            .put("has_art", art(c) != null)
            .put("track", "$title|$artist".hashCode()) // the board refetches the art when this changes
    }

    /** Album art as a small baseline JPEG, which the board's decoder can draw. */
    fun artJpeg(context: Context, size: Int): ByteArray? {
        val bitmap = controller(context)?.let { art(it) } ?: return null
        val scaled = Bitmap.createScaledBitmap(bitmap, size, size, true)
        return ByteArrayOutputStream().use { out ->
            scaled.compress(Bitmap.CompressFormat.JPEG, 85, out)
            if (scaled !== bitmap) scaled.recycle()
            out.toByteArray()
        }
    }

    /** Play/pause, next, previous through the session; volume through the audio system. */
    fun control(context: Context, action: String): String {
        val c = controller(context)
        val controls = c?.transportControls ?: return PhoneActions.media(context, action) // fall back to media keys
        return when (action) {
            "play_pause" -> if (c.playbackState?.state == PlaybackState.STATE_PLAYING) {
                controls.pause(); "Paused"
            } else {
                controls.play(); "Playing"
            }
            "next" -> { controls.skipToNext(); "Next track" }
            "previous" -> { controls.skipToPrevious(); "Previous track" }
            else -> PhoneActions.media(context, action)
        }
    }
}

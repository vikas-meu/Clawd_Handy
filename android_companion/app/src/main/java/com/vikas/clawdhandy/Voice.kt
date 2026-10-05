package com.vikas.clawdhandy

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import android.speech.tts.TextToSpeech
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Push-to-talk for the handheld: the board has no microphone, so the phone listens.
 * Works from the background service as long as the app started it while open (Android only
 * lets a foreground service use the mic if it was started from the screen).
 */
object Voice {
    data class Heard(val text: String, val error: String)

    /** Listens once, until the speaker pauses. Blocking: call it off the main thread. */
    fun listen(context: Context, timeoutMs: Long = 25_000): Heard {
        val app = context.applicationContext
        if (app.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            return Heard("", "Allow the microphone in the Clawd Phone app")
        }
        if (!SpeechRecognizer.isRecognitionAvailable(app)) return Heard("", "This phone has no speech recognition")
        val done = CountDownLatch(1)
        var heard = Heard("", "I didn't hear anything")
        val main = Handler(Looper.getMainLooper())
        var recognizer: SpeechRecognizer? = null
        main.post {
            try {
                val r = SpeechRecognizer.createSpeechRecognizer(app)
                recognizer = r
                r.setRecognitionListener(object : RecognitionListener {
                    override fun onResults(results: Bundle?) {
                        val text = results?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)?.firstOrNull().orEmpty()
                        heard = if (text.isBlank()) Heard("", "I didn't catch that") else Heard(text, "")
                        done.countDown()
                    }

                    override fun onError(error: Int) {
                        heard = Heard("", errorText(error))
                        done.countDown()
                    }

                    override fun onReadyForSpeech(params: Bundle?) = Unit
                    override fun onBeginningOfSpeech() = Unit
                    override fun onRmsChanged(rmsdB: Float) = Unit
                    override fun onBufferReceived(buffer: ByteArray?) = Unit
                    override fun onEndOfSpeech() = Unit
                    override fun onPartialResults(partialResults: Bundle?) = Unit
                    override fun onEvent(eventType: Int, params: Bundle?) = Unit
                })
                r.startListening(Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
                    putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
                    putExtra(RecognizerIntent.EXTRA_LANGUAGE, Locale.getDefault().toLanguageTag())
                    putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 1)
                    putExtra(RecognizerIntent.EXTRA_CALLING_PACKAGE, app.packageName)
                })
            } catch (e: Exception) {
                heard = Heard("", "The phone couldn't start listening")
                done.countDown()
            }
        }
        if (!done.await(timeoutMs, TimeUnit.MILLISECONDS)) heard = Heard("", "Listening timed out")
        main.post { runCatching { recognizer?.destroy() } }
        return heard
    }

    private fun errorText(error: Int) = when (error) {
        SpeechRecognizer.ERROR_NO_MATCH, SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> "I didn't catch that"
        SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> "Allow the microphone in the Clawd Phone app"
        SpeechRecognizer.ERROR_NETWORK, SpeechRecognizer.ERROR_NETWORK_TIMEOUT -> "Speech recognition needs internet"
        SpeechRecognizer.ERROR_RECOGNIZER_BUSY -> "The phone is busy listening already"
        SpeechRecognizer.ERROR_AUDIO -> "The phone's microphone is in use"
        SpeechRecognizer.ERROR_CLIENT -> "Open the Clawd Phone app once, then try again"
        else -> "Listening failed (error $error)"
    }

    // ---- speaking answers out loud on the phone (the board's speaker only plays tones)
    private var tts: TextToSpeech? = null
    @Volatile private var ttsReady = false

    fun startSpeech(context: Context) {
        if (tts != null) return
        tts = TextToSpeech(context.applicationContext) { status -> ttsReady = status == TextToSpeech.SUCCESS }
    }

    fun stopSpeech() {
        tts?.shutdown()
        tts = null
        ttsReady = false
    }

    fun speak(text: String, languageTag: String = "") {
        val engine = tts ?: return
        if (!ttsReady || text.isBlank()) return
        val locale = if (languageTag.isBlank()) Locale.getDefault() else Locale.forLanguageTag(languageTag)
        if (engine.isLanguageAvailable(locale) >= TextToSpeech.LANG_AVAILABLE) engine.language = locale
        engine.speak(text, TextToSpeech.QUEUE_FLUSH, null, "clawd")
    }
}

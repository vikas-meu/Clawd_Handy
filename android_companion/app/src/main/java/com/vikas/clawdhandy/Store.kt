package com.vikas.clawdhandy

import android.content.Context
import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID

/** Everything the app remembers, in one private SharedPreferences file. */
object Store {
    const val PREFS = "clawd_phone"
    const val KEY_API = "anthropic_api_key" // encrypted, see PhoneSecrets
    private const val KEY_TOKEN = "pairing_token"
    private const val KEY_JOURNAL = "journal"
    private const val KEY_NOTES = "notes"
    private const val KEY_ACTIONS = "board_actions"
    private const val MAX_JOURNAL = 40

    /** Called (on any thread) whenever the journal, notes or board link change. */
    @Volatile var onChange: (() -> Unit)? = null

    fun prefs(context: Context): SharedPreferences =
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun pairingToken(context: Context): String {
        val prefs = prefs(context)
        prefs.getString(KEY_TOKEN, null)?.takeIf { it.isNotBlank() }?.let { return it }
        // 16 hex characters: short enough to type into the sketch, impractical to guess
        val token = UUID.randomUUID().toString().replace("-", "").take(16)
        prefs.edit().putString(KEY_TOKEN, token).apply()
        return token
    }

    fun newPairingToken(context: Context): String {
        prefs(context).edit().remove(KEY_TOKEN).apply()
        return pairingToken(context)
    }

    // ---- settings
    fun speakAnswers(context: Context) = prefs(context).getBoolean("speak_answers", true)
    fun setSpeakAnswers(context: Context, on: Boolean) = prefs(context).edit().putBoolean("speak_answers", on).apply()
    fun translateTo(context: Context) = prefs(context).getString("translate_to", "Hindi") ?: "Hindi"
    fun setTranslateTo(context: Context, lang: String) = prefs(context).edit().putString("translate_to", lang).apply()

    // ---- today's Claude use, counted per day
    @Synchronized
    fun recordUsage(context: Context, input: Long, output: Long) {
        val prefs = prefs(context)
        val today = today()
        val same = prefs.getString("usage_day", "") == today
        prefs.edit()
            .putString("usage_day", today)
            .putLong("usage_requests", (if (same) prefs.getLong("usage_requests", 0) else 0) + 1)
            .putLong("usage_input", (if (same) prefs.getLong("usage_input", 0) else 0) + input)
            .putLong("usage_output", (if (same) prefs.getLong("usage_output", 0) else 0) + output)
            .apply()
    }

    fun usageToday(context: Context): Triple<Long, Long, Long> {
        val prefs = prefs(context)
        if (prefs.getString("usage_day", "") != today()) return Triple(0, 0, 0)
        return Triple(prefs.getLong("usage_requests", 0), prefs.getLong("usage_input", 0), prefs.getLong("usage_output", 0))
    }

    private fun today(): String = SimpleDateFormat("yyyyMMdd", Locale.US).format(Date())

    // ---- journal: every question and answer, from the handheld or this app
    @Synchronized
    fun addJournal(context: Context, source: String, mode: String, heard: String, answer: String, failed: Boolean) {
        val old = readArray(context, KEY_JOURNAL)
        val list = JSONArray()
        list.put(
            JSONObject().put("at", System.currentTimeMillis()).put("source", source).put("mode", mode)
                .put("heard", heard).put("answer", answer).put("failed", failed)
        )
        for (i in 0 until minOf(old.length(), MAX_JOURNAL - 1)) list.put(old.get(i))
        prefs(context).edit().putString(KEY_JOURNAL, list.toString()).apply()
        onChange?.invoke()
    }

    fun journal(context: Context): JSONArray = readArray(context, KEY_JOURNAL)

    fun clearJournal(context: Context) {
        prefs(context).edit().remove(KEY_JOURNAL).apply()
        onChange?.invoke()
    }

    // ---- notes and reminders
    @Synchronized
    fun addNote(context: Context, text: String, remindAt: Long): Long {
        val notes = readArray(context, KEY_NOTES)
        val id = System.currentTimeMillis()
        notes.put(JSONObject().put("id", id).put("text", text).put("remindAt", remindAt))
        prefs(context).edit().putString(KEY_NOTES, notes.toString()).apply()
        onChange?.invoke()
        return id
    }

    @Synchronized
    fun updateNotes(context: Context, change: (JSONObject) -> JSONObject?) {
        val old = readArray(context, KEY_NOTES)
        val notes = JSONArray()
        for (i in 0 until old.length()) {
            val note = old.optJSONObject(i) ?: continue
            change(note)?.let { notes.put(it) }
        }
        prefs(context).edit().putString(KEY_NOTES, notes.toString()).apply()
        onChange?.invoke()
    }

    fun notes(context: Context): JSONArray = readArray(context, KEY_NOTES)

    // ---- actions for the handheld, said into this app; the board collects them on its next poll
    @Synchronized
    fun queueBoardAction(context: Context, action: JSONObject) {
        val actions = readArray(context, KEY_ACTIONS)
        actions.put(action)
        prefs(context).edit().putString(KEY_ACTIONS, actions.toString()).apply()
    }

    @Synchronized
    fun takeBoardActions(context: Context): JSONArray {
        val actions = readArray(context, KEY_ACTIONS)
        if (actions.length() > 0) prefs(context).edit().remove(KEY_ACTIONS).apply()
        return actions
    }

    private fun readArray(context: Context, key: String): JSONArray =
        runCatching { JSONArray(prefs(context).getString(key, "[]")) }.getOrDefault(JSONArray())
}

/** The handheld's font only has ASCII, so swap common symbols and drop the rest. */
object BoardText {
    private val replacements = mapOf(
        "×" to "x", "÷" to "/", "−" to "-", "–" to "-", "—" to "-", "‘" to "'", "’" to "'",
        "“" to "\"", "”" to "\"", "…" to "...", "√" to "sqrt", "π" to "pi", "²" to "^2", "³" to "^3",
        "°" to " deg", "≈" to "~", "≠" to "!=", "≤" to "<=", "≥" to ">=", "→" to "->", "½" to "1/2",
        "¼" to "1/4", "¾" to "3/4", "₹" to "Rs", "€" to "EUR", "£" to "GBP", "•" to "-"
    )

    fun ascii(text: String): String {
        var out = text
        for ((from, to) in replacements) out = out.replace(from, to)
        out = java.text.Normalizer.normalize(out, java.text.Normalizer.Form.NFD) // é -> e + accent
        out = out.replace(Regex("[*_`#]+"), "") // stray markdown
        out = out.filter { it.code in 32..126 || it == '\n' }
        return out.replace(Regex("\\s+"), " ").trim()
    }
}

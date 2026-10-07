package com.vikas.clawdhandy

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.text.InputType
import android.text.format.DateUtils
import android.view.Gravity
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import java.util.Locale

/**
 * Clawd Phone: the talk screen, the link to the handheld, and setup. Built in code (no XML
 * layouts) to keep the project small.
 */
class MainActivity : Activity() {
    // Claude palette, same as the handheld
    private val dark = Color.rgb(20, 20, 19)
    private val panel = Color.rgb(34, 33, 31)
    private val raised = Color.rgb(46, 45, 42)
    private val cream = Color.rgb(250, 249, 245)
    private val muted = Color.rgb(150, 146, 139)
    private val orange = Color.rgb(217, 119, 87)
    private val green = Color.rgb(76, 175, 80)
    private val red = Color.rgb(229, 72, 77)

    private val modes = listOf("ask" to "Ask", "translate" to "Translate", "note" to "Note")
    private val languages = listOf(
        "English", "Hindi", "Spanish", "French", "German", "Japanese", "Chinese", "Arabic",
        "Bengali", "Tamil", "Telugu", "Marathi", "Italian", "Korean", "Russian"
    )
    private var mode = "ask"
    private var busy = false
    private val ui = Handler(Looper.getMainLooper())

    private lateinit var linkPill: TextView
    private lateinit var modeRow: LinearLayout
    private lateinit var langButton: TextView
    private lateinit var heardText: TextView
    private lateinit var answerText: TextView
    private lateinit var talkButton: TextView
    private lateinit var typeBox: EditText
    private lateinit var handheldStatus: TextView
    private lateinit var linkDetail: TextView
    private lateinit var tokenText: TextView
    private lateinit var usageText: TextView
    private lateinit var locationText: TextView
    private lateinit var journalList: LinearLayout
    private lateinit var notesList: LinearLayout
    private lateinit var setupList: LinearLayout

    private val ticker = object : Runnable {
        override fun run() {
            refreshStatus()
            ui.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = dark
        window.navigationBarColor = dark
        Store.pairingToken(this)
        Voice.startSpeech(this)
        build()
        askFirstPermissions()
    }

    override fun onResume() {
        super.onResume()
        PhoneActions.appInForeground = true
        Voice.startSpeech(this)
        Store.onChange = { runOnUiThread { refreshAll() } }
        refreshAll()
        ui.post(ticker)
    }

    override fun onPause() {
        PhoneActions.appInForeground = false
        Store.onChange = null
        ui.removeCallbacks(ticker)
        super.onPause()
    }

    // ================================================================== layout

    private fun build() {
        val scroll = ScrollView(this).apply { setBackgroundColor(dark); isFillViewport = true }
        val page = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(18), dp(20), dp(18), dp(36))
        }
        scroll.addView(page)
        setContentView(scroll)

        // Top bar: wordmark + link pill
        val top = row()
        top.addView(View(this).apply { background = rounded(orange, 3) }, LinearLayout.LayoutParams(dp(14), dp(14)).apply { rightMargin = dp(8) })
        top.addView(label("clawd", 22, cream, bold = true), LinearLayout.LayoutParams(0, -2, 1f))
        linkPill = label("", 12, cream, bold = true).apply {
            setPadding(dp(12), dp(6), dp(12), dp(6))
            setOnClickListener { toggleLink() }
        }
        top.addView(linkPill)
        page.addView(top)
        page.addView(label("Your pocket companion, on your phone and your MEMENTO.", 13, muted), margins(top = 4, bottom = 16))

        page.addView(buildTalkCard(), margins(bottom = 14))
        section(page, "HANDHELD")
        page.addView(buildLinkCard(), margins(bottom = 14))
        section(page, "WHERE YOU ARE")
        page.addView(buildLocationCard(), margins(bottom = 14))
        section(page, "JOURNAL")
        journalList = column()
        page.addView(card().apply { addView(journalList) }, margins(bottom = 14))
        section(page, "NOTES & REMINDERS")
        page.addView(buildNotesCard(), margins(bottom = 14))
        section(page, "SETUP")
        setupList = column()
        page.addView(card().apply { addView(setupList) })
        page.addView(
            label("Fan-made, not affiliated with Anthropic. Claude API use is billed to your own API key.", 11, muted),
            margins(top = 18)
        )
    }

    private fun buildTalkCard(): LinearLayout {
        val c = card(border = orange)
        modeRow = row()
        c.addView(modeRow)
        langButton = label("", 14, orange, bold = true).apply {
            setPadding(0, dp(10), 0, 0)
            setOnClickListener { pickLanguage() }
        }
        c.addView(langButton)
        heardText = label("", 13, muted)
        c.addView(heardText, margins(top = 14))
        answerText = label("Tap the button and talk. Ask anything, translate, or save a note.", 20, cream)
        answerText.setLineSpacing(0f, 1.15f)
        c.addView(answerText, margins(top = 4))

        // The big push-to-talk button
        val holder = FrameLayout(this)
        talkButton = label("TALK", 16, dark, bold = true).apply {
            gravity = Gravity.CENTER
            background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(orange) }
            setOnClickListener { talk() }
        }
        holder.addView(talkButton, FrameLayout.LayoutParams(dp(104), dp(104), Gravity.CENTER))
        c.addView(holder, margins(top = 20, bottom = 12))

        // Or type
        val typeRow = row()
        typeBox = EditText(this).apply {
            hint = "or type here"
            setHintTextColor(muted)
            setTextColor(cream)
            textSize = 15f
            background = rounded(raised, 12)
            setPadding(dp(14), dp(10), dp(14), dp(10))
            imeOptions = EditorInfo.IME_ACTION_SEND
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
            setSingleLine()
            setOnEditorActionListener { _, _, _ -> sendTyped(); true }
        }
        typeRow.addView(typeBox, LinearLayout.LayoutParams(0, dp(46), 1f))
        typeRow.addView(pillButton("Send", primary = false) { sendTyped() }, LinearLayout.LayoutParams(-2, dp(46)).apply { leftMargin = dp(8) })
        c.addView(typeRow)
        renderModes()
        return c
    }

    private fun buildLinkCard(): LinearLayout {
        val c = card()
        handheldStatus = label("", 17, cream, bold = true)
        c.addView(handheldStatus)
        linkDetail = label("", 13, muted)
        c.addView(linkDetail, margins(top = 4))
        usageText = label("", 13, muted)
        c.addView(usageText, margins(top = 4))
        c.addView(pillButton("Pair handheld", primary = true) { pairHandheld() }, LinearLayout.LayoutParams(-1, dp(46)).apply { topMargin = dp(12) })
        c.addView(label("Tap, then switch on (or reset) the handheld. It pairs by itself within a few seconds.", 12, muted), margins(top = 6))
        c.addView(label("MANUAL PAIRING TOKEN", 11, muted, bold = true), margins(top = 14))
        tokenText = label("", 17, cream, bold = true).apply { typeface = Typeface.MONOSPACE }
        c.addView(tokenText, margins(top = 2))
        c.addView(label("Only needed if pairing doesn't work: BRIDGE_TOKEN in the sketch.", 12, muted), margins(top = 2))
        val buttons = row()
        buttons.addView(pillButton("Copy token", primary = false) { copyToken() }, LinearLayout.LayoutParams(0, dp(42), 1f))
        buttons.addView(pillButton("New token", primary = false) { confirmNewToken() }, LinearLayout.LayoutParams(0, dp(42), 1f).apply { leftMargin = dp(8) })
        c.addView(buttons, margins(top = 8))
        return c
    }

    private fun buildLocationCard(): LinearLayout {
        val c = card()
        locationText = label("", 15, cream)
        c.addView(locationText)
        val buttons = row()
        buttons.addView(pillButton("Update now", primary = false) { updateLocation() }, LinearLayout.LayoutParams(0, dp(44), 1f))
        buttons.addView(pillButton("Nearby map", primary = false) { openMap() }, LinearLayout.LayoutParams(0, dp(44), 1f).apply { leftMargin = dp(8) })
        c.addView(buttons, margins(top = 10))
        c.addView(label("The handheld uses this for weather, planes overhead and \"near me\" questions.", 12, muted), margins(top = 8))
        return c
    }

    private fun buildNotesCard(): LinearLayout {
        val c = card()
        notesList = column()
        c.addView(notesList)
        c.addView(pillButton("Add a note", primary = false) { noteDialog() }, LinearLayout.LayoutParams(-1, dp(44)).apply { topMargin = dp(10) })
        return c
    }

    // ================================================================== talking

    private fun renderModes() {
        modeRow.removeAllViews()
        for ((key, name) in modes) {
            val on = key == mode
            val chip = label(name, 14, if (on) dark else cream, bold = true).apply {
                gravity = Gravity.CENTER
                background = rounded(if (on) orange else raised, 18)
                setOnClickListener { if (!busy) { mode = key; renderModes() } }
            }
            modeRow.addView(chip, LinearLayout.LayoutParams(0, dp(36), 1f).apply { if (key != "ask") leftMargin = dp(6) })
        }
        langButton.visibility = if (mode == "translate") View.VISIBLE else View.GONE
        langButton.text = "Into ${Store.translateTo(this)}  ▾"
    }

    private fun pickLanguage() {
        AlertDialog.Builder(this).setTitle("Translate into")
            .setItems(languages.toTypedArray()) { _, i ->
                Store.setTranslateTo(this, languages[i])
                renderModes()
            }.show()
    }

    private fun talk() {
        if (busy) return
        if (!ready()) return
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQUEST_MIC)
            return
        }
        setBusy("Listening…", "LISTENING")
        Thread {
            val heard = Voice.listen(this)
            if (heard.error.isNotEmpty()) {
                runOnUiThread { showAnswer("", heard.error, failed = true) }
            } else {
                runOnUiThread { heardText.text = "You: ${heard.text}" }
                answer(heard.text)
            }
        }.start()
    }

    private fun sendTyped() {
        val text = typeBox.text.toString().trim()
        if (text.isEmpty() || busy || !ready()) return
        typeBox.setText("")
        heardText.text = "You: $text"
        setBusy(null, "…")
        Thread { answer(text) }.start()
    }

    /** Off the main thread: ask Claude (or save the note), show and speak the answer. */
    private fun answer(text: String) {
        runOnUiThread { setBusy(if (mode == "note") "Saving…" else "Thinking…", "…") }
        val reply = Brain.get(this).chat(text, mode, Store.translateTo(this), "app")
        Store.addJournal(this, "app", mode, text, reply.answer, !reply.ok)
        if (reply.ok && reply.speak.isNotBlank() && Store.speakAnswers(this)) Voice.speak(reply.speak, reply.speakLang)
        runOnUiThread { showAnswer("You: $text", reply.answer, !reply.ok) }
    }

    private fun ready(): Boolean {
        if (mode != "note" && runCatching { PhoneSecrets.readApiKey(this).isBlank() }.getOrDefault(true)) {
            apiKeyDialog()
            return false
        }
        return true
    }

    private fun setBusy(answer: String?, button: String) {
        busy = true
        if (answer != null) answerText.text = answer
        answerText.setTextColor(muted)
        talkButton.text = button
        talkButton.background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(raised); setStroke(dp(3), orange) }
        talkButton.setTextColor(orange)
    }

    private fun showAnswer(heard: String, answer: String, failed: Boolean) {
        busy = false
        if (heard.isNotEmpty()) heardText.text = heard
        answerText.text = answer
        answerText.setTextColor(if (failed) red else cream)
        talkButton.text = "TALK"
        talkButton.background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(orange) }
        talkButton.setTextColor(dark)
    }

    // ================================================================== refresh

    private fun refreshAll() {
        refreshStatus()
        refreshJournal()
        refreshNotes()
        refreshSetup()
    }

    private fun refreshStatus() {
        val on = PhoneBridgeService.running
        linkPill.text = if (on) "●  LINK ON" else "○  LINK OFF"
        linkPill.setTextColor(if (on) dark else cream)
        linkPill.background = rounded(if (on) green else raised, 16)

        val seen = PhoneBridgeService.boardSeenAt
        val pairing = PhoneBridgeService.pairingSecondsLeft()
        handheldStatus.text = when {
            !on -> "Link is off"
            pairing > 0 -> "Pairing open %d:%02d… switch on the handheld".format(pairing / 60, pairing % 60)
            seen == 0L -> "Waiting for your handheld…"
            System.currentTimeMillis() - seen < 90_000 -> "Handheld connected"
            else -> "Handheld last seen ${DateUtils.getRelativeTimeSpanString(seen)}"
        }
        handheldStatus.setTextColor(
            if (on && pairing > 0) orange else if (on && seen > 0 && System.currentTimeMillis() - seen < 90_000) green else cream
        )
        linkDetail.text = buildString {
            append("This phone: ${phoneAddresses()}  ·  port ${PhoneBridgeService.HTTP_PORT}")
            if (PhoneBridgeService.boardAddress.isNotEmpty()) append("\nHandheld: ${PhoneBridgeService.boardAddress}")
            if (!on) append("\nTap LINK OFF to start. The handheld can join the same WiFi or this phone's hotspot.")
            else if (!PhoneBridgeService.micEnabled) append("\nPush-to-talk is off: allow the microphone, then restart the link.")
        }
        val (requests, input, output) = Store.usageToday(this)
        usageText.text = "Claude today: $requests requests · ${compact(input)} in · ${compact(output)} out tokens"
        tokenText.text = Store.pairingToken(this)

        val fix = PhoneLocation.last(this)
        locationText.text = if (fix == null) {
            if (PhoneLocation.hasPermission(this)) "No fix yet. Tap Update now." else "Location is off. Allow it in Setup below."
        } else {
            val place = fix.city.ifBlank { "Unknown place" }
            "$place\n%.5f, %.5f  ·  ±%.0f m  ·  %s".format(
                Locale.US, fix.lat, fix.lon, fix.accuracy, DateUtils.getRelativeTimeSpanString(fix.at)
            )
        }
    }

    private fun refreshJournal() {
        journalList.removeAllViews()
        val journal = Store.journal(this)
        if (journal.length() == 0) {
            journalList.addView(label("Everything you ask, from the handheld or here, shows up here.", 13, muted))
            return
        }
        for (i in 0 until minOf(journal.length(), 10)) {
            val e = journal.optJSONObject(i) ?: continue
            val item = column()
            val where = if (e.optString("source") == "board") "HANDHELD" else "PHONE"
            val tag = "$where · ${e.optString("mode").uppercase(Locale.US)} · ${DateUtils.getRelativeTimeSpanString(e.optLong("at"))}"
            item.addView(label(tag, 10, orange, bold = true))
            if (e.optString("heard").isNotBlank()) item.addView(label(e.optString("heard"), 13, muted), margins(top = 2))
            item.addView(label(e.optString("answer"), 14, if (e.optBoolean("failed")) red else cream), margins(top = 2))
            journalList.addView(item, margins(top = if (i == 0) 0 else 12))
        }
        journalList.addView(label("Clear journal", 13, orange, bold = true).apply {
            setPadding(0, dp(12), 0, 0)
            setOnClickListener { Store.clearJournal(this@MainActivity); Brain.get(this@MainActivity).forget() }
        })
    }

    private fun refreshNotes() {
        notesList.removeAllViews()
        val notes = Store.notes(this)
        if (notes.length() == 0) {
            notesList.addView(label("Say \"Note\" mode on the handheld, or ask Clawd to remind you of something.", 13, muted))
            return
        }
        for (i in notes.length() - 1 downTo 0) {
            val note = notes.optJSONObject(i) ?: continue
            val line = row().apply { setPadding(0, dp(6), 0, dp(6)) }
            val text = column()
            text.addView(label(note.optString("text"), 15, cream))
            val at = note.optLong("remindAt")
            if (at > System.currentTimeMillis()) text.addView(label("⏰ ${DateUtils.getRelativeTimeSpanString(at)}", 12, orange))
            line.addView(text, LinearLayout.LayoutParams(0, -2, 1f))
            line.addView(label("✕", 18, muted).apply {
                setPadding(dp(14), dp(4), dp(4), dp(4))
                setOnClickListener {
                    val id = note.optLong("id")
                    ReminderReceiver.cancel(this@MainActivity, id)
                    Store.updateNotes(this@MainActivity) { if (it.optLong("id") == id) null else it }
                }
            })
            notesList.addView(line)
        }
    }

    private fun refreshSetup() {
        setupList.removeAllViews()
        val hasKey = runCatching { PhoneSecrets.readApiKey(this).isNotBlank() }.getOrDefault(false)
        setupRow("Anthropic API key", hasKey, if (hasKey) "Change" else "Add") { apiKeyDialog() }
        setupRow("Microphone (push-to-talk)", granted(Manifest.permission.RECORD_AUDIO), "Allow") {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQUEST_MIC)
        }
        setupRow("Location (weather, near me)", PhoneLocation.hasPermission(this), "Allow") {
            requestPermissions(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION), REQUEST_LOCATION)
        }
        if (android.os.Build.VERSION.SDK_INT >= 33) {
            setupRow("Notifications (reminders)", granted(Manifest.permission.POST_NOTIFICATIONS), "Allow") {
                requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATIONS)
            }
        }
        setupRow("Now Playing on the handheld", NowPlaying.allowed(this), "Allow") {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
        setupRow("Open apps from the handheld", Settings.canDrawOverlays(this), "Allow") {
            startActivity(Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:$packageName")))
        }
        val speak = row().apply { setPadding(0, dp(8), 0, 0) }
        speak.addView(label("Speak answers out loud", 15, cream), LinearLayout.LayoutParams(0, -2, 1f))
        speak.addView(Switch(this).apply {
            isChecked = Store.speakAnswers(this@MainActivity)
            setOnCheckedChangeListener { _, on -> Store.setSpeakAnswers(this@MainActivity, on) }
        })
        setupList.addView(speak)
    }

    private fun setupRow(name: String, done: Boolean, action: String, onClick: () -> Unit) {
        val line = row().apply { setPadding(0, dp(6), 0, dp(6)) }
        line.addView(label(if (done) "✓" else "○", 16, if (done) green else muted, bold = true), LinearLayout.LayoutParams(dp(26), -2))
        line.addView(label(name, 15, cream), LinearLayout.LayoutParams(0, -2, 1f))
        if (!done || action == "Change") line.addView(label(action, 14, orange, bold = true).apply { setOnClickListener { onClick() } })
        setupList.addView(line)
    }

    // ================================================================== actions

    private fun toggleLink() {
        if (PhoneBridgeService.running) {
            PhoneBridgeService.stop(this)
        } else {
            PhoneBridgeService.start(this)
            PhoneBridgeService.openPairing() // switching the link on is a good moment to pair
            toast("Link on. Your handheld will find this phone by itself.")
        }
        ui.postDelayed({ refreshAll() }, 400)
    }

    private fun pairHandheld() {
        if (!PhoneBridgeService.running) PhoneBridgeService.start(this)
        PhoneBridgeService.openPairing()
        toast("Pairing open for 2 minutes. Switch on or reset the handheld.")
        ui.postDelayed({ refreshAll() }, 400)
    }

    private fun copyToken() {
        getSystemService(ClipboardManager::class.java).setPrimaryClip(ClipData.newPlainText("Clawd pairing token", Store.pairingToken(this)))
        toast("Copied. Paste it as BRIDGE_TOKEN in the sketch")
    }

    private fun confirmNewToken() {
        AlertDialog.Builder(this).setTitle("Make a new pairing token?")
            .setMessage("The handheld will stop working until you pair it again (Pair handheld).")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("New token") { _, _ -> Store.newPairingToken(this); refreshStatus() }
            .show()
    }

    private fun updateLocation() {
        if (!PhoneLocation.hasPermission(this)) {
            requestPermissions(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION), REQUEST_LOCATION)
            return
        }
        locationText.text = "Finding you…"
        Thread {
            val fix = PhoneLocation.current(this, maxAgeMs = 0)
            runOnUiThread { if (fix == null) locationText.text = "No fix. Is location turned on?" else refreshStatus() }
        }.start()
    }

    private fun openMap() {
        val fix = PhoneLocation.last(this) ?: return updateLocation()
        runCatching {
            startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("geo:${fix.lat},${fix.lon}?q=${Uri.encode("things to do")}")))
        }.onFailure { toast("No map app found") }
    }

    private fun apiKeyDialog() {
        val input = EditText(this).apply {
            hint = "sk-ant-..."
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        val box = FrameLayout(this).apply { setPadding(dp(20), dp(8), dp(20), 0); addView(input) }
        AlertDialog.Builder(this)
            .setTitle("Anthropic API key")
            .setMessage("Get one at console.anthropic.com. It's encrypted with the Android Keystore and only sent to Anthropic.")
            .setView(box)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Save") { _, _ ->
                try {
                    PhoneSecrets.saveApiKey(this, input.text.toString().trim())
                    toast("API key saved")
                } catch (_: Exception) {
                    toast("Couldn't save the key securely")
                }
                refreshSetup()
            }.show()
    }

    private fun noteDialog() {
        val input = EditText(this).apply { hint = "Write a note"; minLines = 2; gravity = Gravity.TOP }
        val box = FrameLayout(this).apply { setPadding(dp(20), dp(8), dp(20), 0); addView(input) }
        fun save(minutes: Int) {
            val text = input.text.toString().trim()
            if (text.isEmpty()) return toast("Write something first")
            if (minutes > 0) PhoneActions.remind(this, text, minutes) else Store.addNote(this, text, 0)
        }
        AlertDialog.Builder(this).setTitle("New note").setView(box)
            .setPositiveButton("Save") { _, _ -> save(0) }
            .setNeutralButton("Remind in 1 h") { _, _ -> save(60) }
            .setNegativeButton("Remind in 15 min") { _, _ -> save(15) }
            .show()
    }

    // ================================================================== permissions

    private fun askFirstPermissions() {
        val wanted = mutableListOf(
            Manifest.permission.RECORD_AUDIO,
            Manifest.permission.ACCESS_FINE_LOCATION,
            Manifest.permission.ACCESS_COARSE_LOCATION
        )
        if (android.os.Build.VERSION.SDK_INT >= 33) wanted += Manifest.permission.POST_NOTIFICATIONS
        val missing = wanted.filterNot { granted(it) }
        if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), REQUEST_FIRST)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        refreshSetup()
        // The link only gets mic/location access if it starts after they're granted
        if (PhoneBridgeService.running && grantResults.any { it == PackageManager.PERMISSION_GRANTED }) {
            PhoneBridgeService.stop(this)
            ui.postDelayed({ PhoneBridgeService.start(this) }, 300)
        }
        if (requestCode == REQUEST_LOCATION && PhoneLocation.hasPermission(this)) updateLocation()
    }

    private fun granted(permission: String) = checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED

    // ================================================================== small view helpers

    /** The phone's local addresses: its WiFi address, plus its hotspot address when sharing one. */
    private fun phoneAddresses(): String {
        @Suppress("DEPRECATION")
        val wifi = getSystemService(WifiManager::class.java).connectionInfo.ipAddress
        val wifiIp = if (wifi == 0) "" else "${wifi and 0xff}.${wifi shr 8 and 0xff}.${wifi shr 16 and 0xff}.${wifi shr 24 and 0xff}"
        val mobileData = listOf("rmnet", "ccmni", "dummy", "tun", "ipsec")
        val local = runCatching {
            java.net.NetworkInterface.getNetworkInterfaces().toList()
                .filter { it.isUp && !it.isLoopback && mobileData.none { prefix -> it.name.startsWith(prefix) } }
                .flatMap { iface -> iface.inetAddresses.toList().filterIsInstance<java.net.Inet4Address>() }
                .mapNotNull { it.hostAddress }
        }.getOrDefault(emptyList())
        if (local.isEmpty()) return "not on WiFi or hotspot"
        return local.distinct().joinToString("  ·  ") { if (it == wifiIp) it else "$it (hotspot)" }
    }

    private fun compact(n: Long) = when {
        n >= 1_000_000 -> "%.1fM".format(Locale.US, n / 1e6)
        n >= 1_000 -> "%.1fk".format(Locale.US, n / 1e3)
        else -> n.toString()
    }

    private fun section(parent: LinearLayout, title: String) =
        parent.addView(label(title, 11, muted, bold = true).apply { letterSpacing = 0.12f }, margins(bottom = 6))

    private fun card(border: Int? = null) = column().apply {
        setPadding(dp(16), dp(16), dp(16), dp(16))
        background = rounded(panel, 20).apply { if (border != null) setStroke(dp(1), border) }
    }

    private fun column() = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

    private fun row() = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    private fun label(value: String, size: Int, color: Int, bold: Boolean = false) = TextView(this).apply {
        text = value
        textSize = size.toFloat()
        setTextColor(color)
        if (bold) setTypeface(typeface, Typeface.BOLD)
    }

    private fun pillButton(value: String, primary: Boolean, action: () -> Unit) = Button(this).apply {
        text = value
        isAllCaps = false
        textSize = 14f
        setTextColor(if (primary) dark else cream)
        background = rounded(if (primary) orange else raised, 14)
        setOnClickListener { action() }
    }

    private fun rounded(color: Int, radius: Int) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(radius).toFloat()
    }

    private fun margins(top: Int = 0, bottom: Int = 0) = LinearLayout.LayoutParams(-1, -2).apply {
        topMargin = dp(top)
        bottomMargin = dp(bottom)
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    private fun toast(message: String) = Toast.makeText(this, message, Toast.LENGTH_SHORT).show()

    companion object {
        private const val REQUEST_FIRST = 100
        private const val REQUEST_LOCATION = 101
        private const val REQUEST_MIC = 102
        private const val REQUEST_NOTIFICATIONS = 103
    }
}

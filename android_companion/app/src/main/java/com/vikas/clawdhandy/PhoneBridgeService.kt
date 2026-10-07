package com.vikas.clawdhandy

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.BitmapFactory
import android.net.wifi.WifiManager
import android.os.BatteryManager
import android.os.Build
import android.os.IBinder
import com.google.android.gms.tasks.Tasks
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.face.FaceDetection
import com.google.mlkit.vision.face.FaceDetectorOptions
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.ByteArrayOutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.URLDecoder
import java.security.MessageDigest
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * The handheld's link to the phone, on your local WiFi. Replaces the old PC bridge:
 *
 *   GET  /usage                       today's Claude use, phone battery/city, queued actions
 *   GET  /location                    phone GPS (fresh fix, cached 2 min)
 *   POST /listen                      the phone listens once; returns what you said
 *   POST /ask/text   {prompt, mode, lang, source}   ask / translate / note
 *   POST /ask/image?mode=look|translate&lang=..     body = JPEG
 *   POST /face, /qr                   body = JPEG, found on the phone with ML Kit
 *   GET  /nowplaying                  what's playing on the phone; /nowplaying/art = 120x120 JPEG
 *   POST /media?action=play_pause|next|previous|volume_up|volume_down
 *   POST /browse     {input}          a web page (or DuckDuckGo search) as text + links
 *   POST /hunt       {theme, count}   scavenger hunt clues written by Claude
 *   GET  /pair                        the pairing token, only while pairing is open
 *
 * Every other request needs the pairing token in X-Clawd-Token. The board finds the phone by
 * broadcasting "CLAUDE_BRIDGE?" to UDP 8766, or, on the phone's hotspot, at its gateway address.
 */
class PhoneBridgeService : Service() {
    companion object {
        const val HTTP_PORT = 8765
        private const val DISCOVERY_PORT = 8766
        private const val CHANNEL_ID = "clawd_phone_bridge"
        private const val MAX_BODY = 4 * 1024 * 1024 // a 1024x768 JPEG is ~100-300 KB

        @Volatile var running = false
            private set
        @Volatile var boardAddress = ""   // where the handheld last called from
            private set
        @Volatile var boardSeenAt = 0L
            private set
        @Volatile var micEnabled = false // the service may use the microphone (push-to-talk)
            private set
        /** Until when a handheld may fetch the pairing token without having it (0 = closed). */
        @Volatile var pairingOpenUntil = 0L
            private set

        fun openPairing() {
            pairingOpenUntil = System.currentTimeMillis() + 120_000
        }

        fun pairingSecondsLeft(): Long = maxOf(0L, (pairingOpenUntil - System.currentTimeMillis()) / 1000)

        fun start(context: Context) = context.startForegroundService(Intent(context, PhoneBridgeService::class.java))

        fun stop(context: Context) = context.stopService(Intent(context, PhoneBridgeService::class.java))
    }

    @Volatile private var httpSocket: ServerSocket? = null
    @Volatile private var udpSocket: DatagramSocket? = null
    private val clients = Executors.newFixedThreadPool(4)
    private val faceDetector by lazy {
        FaceDetection.getClient(FaceDetectorOptions.Builder().setPerformanceMode(FaceDetectorOptions.PERFORMANCE_MODE_FAST).build())
    }
    private val qrScanner by lazy {
        BarcodeScanning.getClient(BarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build())
    }

    override fun onCreate() {
        super.onCreate()
        startInForeground()
        running = true
        Voice.startSpeech(this)
        Thread(::serveHttp, "clawd-http").start()
        Thread(::serveDiscovery, "clawd-discovery").start()
        Store.onChange?.invoke()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        running = false
        runCatching { httpSocket?.close() }
        runCatching { udpSocket?.close() }
        clients.shutdownNow()
        Voice.stopSpeech()
        Store.onChange?.invoke()
        super.onDestroy()
    }

    private fun startInForeground() {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel(CHANNEL_ID, "Clawd device link", NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        val notification = Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("Clawd is listening for your handheld")
            .setContentText("Phone GPS, voice and Claude for your MEMENTO on this WiFi")
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        // Location and microphone only work in the background if the service declares them, and
        // Android refuses those types without the permission, so add each one only when granted.
        var types = 0
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            if (PhoneLocation.hasPermission(this)) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
        }
        val mic = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && mic) types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        micEnabled = mic
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) startForeground(1, notification, types) else startForeground(1, notification)
        } catch (_: Exception) {
            // Restarted from the background, where location/mic types aren't allowed: keep the link up without them
            micEnabled = false
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
            } else {
                startForeground(1, notification)
            }
        }
    }

    // ------------------------------------------------------------------ network plumbing

    private fun serveHttp() {
        try {
            ServerSocket().use { server ->
                server.reuseAddress = true
                server.bind(InetSocketAddress(HTTP_PORT))
                httpSocket = server
                while (running) {
                    val socket = server.accept()
                    clients.execute { handleClient(socket) }
                }
            }
        } catch (_: Exception) {
            if (running) stopSelf()
        }
    }

    private fun serveDiscovery() {
        val wifi = applicationContext.getSystemService(WIFI_SERVICE) as WifiManager
        val lock = wifi.createMulticastLock("clawd-discovery").apply {
            setReferenceCounted(false)
            acquire()
        }
        try {
            DatagramSocket(null).use { socket ->
                udpSocket = socket
                socket.reuseAddress = true
                socket.broadcast = true
                socket.bind(InetSocketAddress(DISCOVERY_PORT))
                val buffer = ByteArray(64)
                while (running) {
                    val packet = DatagramPacket(buffer, buffer.size)
                    socket.receive(packet)
                    if (String(packet.data, packet.offset, packet.length, Charsets.US_ASCII).trim() == "CLAUDE_BRIDGE?") {
                        val reply = "CLAUDE_BRIDGE $HTTP_PORT".toByteArray(Charsets.US_ASCII)
                        socket.send(DatagramPacket(reply, reply.size, packet.address, packet.port))
                    }
                }
            }
        } catch (_: Exception) {
            // socket closed on stop
        } finally {
            if (lock.isHeld) lock.release()
        }
    }

    private fun handleClient(socket: Socket) {
        socket.use { client ->
            client.soTimeout = 15_000
            val input = BufferedInputStream(client.getInputStream())
            val output = BufferedOutputStream(client.getOutputStream())
            try {
                val requestLine = readLine(input) ?: return
                val parts = requestLine.split(' ')
                if (parts.size < 2) return respond(output, 400, error("Malformed request"))
                val method = parts[0]
                val path = parts[1].substringBefore('?')
                val query = parseQuery(parts[1].substringAfter('?', ""))
                var contentLength = 0
                var token = ""
                while (true) {
                    val line = readLine(input) ?: break
                    if (line.isEmpty()) break
                    val colon = line.indexOf(':')
                    if (colon <= 0) continue
                    val name = line.substring(0, colon).trim()
                    val value = line.substring(colon + 1).trim()
                    if (name.equals("Content-Length", true)) contentLength = value.toIntOrNull() ?: -1
                    else if (name.equals("X-Clawd-Token", true)) token = value
                }
                if (path == "/pair") return respond(output, if (pairingSecondsLeft() > 0) 200 else 403, pair(client))
                // Check the token before reading a body, so strangers on the WiFi can't upload anything
                if (!authorized(token)) return respond(output, 401, error("Not paired. Tap Pair handheld in the Clawd Phone app"))
                if (contentLength < 0 || contentLength > MAX_BODY) return respond(output, 413, error("Request body is too large"))
                val body = readBody(input, contentLength)
                if (!client.inetAddress.isLoopbackAddress) {
                    boardAddress = client.inetAddress.hostAddress ?: ""
                    boardSeenAt = System.currentTimeMillis()
                }
                client.soTimeout = 90_000 // Claude and listening can take a while
                val response = when {
                    method == "GET" && path == "/usage" -> usage()
                    method == "GET" && path == "/location" -> location()
                    path == "/listen" -> listen()
                    method == "POST" && path == "/ask/text" -> askText(body)
                    method == "POST" && path == "/ask/image" -> askImage(body, query)
                    method == "POST" && path == "/face" -> findFaces(body)
                    method == "POST" && path == "/qr" -> readQr(body)
                    method == "GET" && path == "/nowplaying" -> NowPlaying.current(this)
                    method == "GET" && path == "/nowplaying/art" -> {
                        val jpeg = NowPlaying.artJpeg(this, 120) ?: return respond(output, 404, error("No album art"))
                        return respondBytes(output, "image/jpeg", jpeg)
                    }
                    method == "POST" && path == "/media" ->
                        JSONObject().put("ok", true).put("answer", NowPlaying.control(this, query["action"] ?: "play_pause"))
                    method == "POST" && path == "/browse" ->
                        PhoneBrowser.open(JSONObject(String(body, Charsets.UTF_8)).optString("input"))
                    method == "POST" && path == "/hunt" -> hunt(body)
                    else -> return respond(output, 404, error("Unknown endpoint $path"))
                }
                respond(output, 200, response)
            } catch (e: Exception) {
                runCatching { respond(output, 500, error(e.message ?: "Phone request failed")) }
            }
        }
    }

    private fun authorized(received: String): Boolean {
        val expected = Store.pairingToken(this)
        return received.isNotEmpty() && MessageDigest.isEqual(expected.toByteArray(), received.toByteArray())
    }

    // ------------------------------------------------------------------ endpoints

    private fun pair(client: Socket): JSONObject {
        if (pairingSecondsLeft() <= 0) return error("Pairing is closed. Tap Pair handheld in the Clawd Phone app")
        pairingOpenUntil = 0 // one handheld per tap
        boardAddress = client.inetAddress.hostAddress ?: ""
        boardSeenAt = System.currentTimeMillis()
        Store.onChange?.invoke()
        return JSONObject().put("ok", true).put("token", Store.pairingToken(this))
    }

    private fun usage(): JSONObject {
        val (requests, input, output) = Store.usageToday(this)
        val battery = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = battery?.let {
            val l = it.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val s = it.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
            if (l >= 0 && s > 0) l * 100 / s else -1
        } ?: -1
        val status = battery?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL
        Store.onChange?.invoke() // the app shows "handheld seen just now"
        return JSONObject()
            .put("ok", true)
            .put("today", JSONObject().put("requests", requests).put("input_tokens", input).put("output_tokens", output))
            .put(
                "phone", JSONObject().put("battery", level).put("charging", charging)
                    .put("city", PhoneLocation.last(this)?.city ?: "")
            )
            .put("actions", Store.takeBoardActions(this))
    }

    private fun location(): JSONObject {
        val fix = PhoneLocation.current(this)
            ?: return JSONObject().put("ok", false).put("success", false)
                .put("error", if (PhoneLocation.hasPermission(this)) "No GPS fix yet" else "Allow location in the Clawd Phone app")
        return JSONObject()
            .put("ok", true)
            .put("success", true) // same shape as ipwho.is, the board's fallback
            .put("source", "phone")
            .put("city", fix.city)
            .put("latitude", fix.lat)
            .put("longitude", fix.lon)
            .put("accuracy", fix.accuracy.toDouble())
            .put("age_s", (System.currentTimeMillis() - fix.at) / 1000)
    }

    private fun listen(): JSONObject {
        if (!micEnabled) {
            return error("Open the Clawd Phone app, allow the microphone, then restart the link")
        }
        val heard = Voice.listen(this)
        return if (heard.error.isEmpty()) JSONObject().put("ok", true).put("text", BoardText.ascii(heard.text)).put("raw", heard.text)
        else error(heard.error)
    }

    private fun askText(body: ByteArray): JSONObject {
        val request = JSONObject(String(body, Charsets.UTF_8))
        // The board sends back the "raw" text from /listen when it has it, so accents survive
        val prompt = request.optString("raw").ifBlank { request.optString("prompt") }
        val mode = request.optString("mode", "ask")
        val lang = request.optString("lang", Store.translateTo(this))
        val source = request.optString("source", "board")
        val reply = Brain.get(this).chat(prompt, mode, lang, source)
        finish(source, mode, prompt, reply)
        return JSONObject().put("ok", reply.ok).put("answer", reply.answer).put("heard", BoardText.ascii(prompt))
            .put("actions", reply.actions).apply { if (!reply.ok) put("error", reply.answer) }
    }

    private fun askImage(jpeg: ByteArray, query: Map<String, String>): JSONObject {
        val mode = query["mode"] ?: "look"
        val lang = query["lang"] ?: Store.translateTo(this)
        val reply = Brain.get(this).photo(jpeg, mode, lang)
        finish("board", if (mode == "translate") "photo-translate" else "photo", "(photo)", reply)
        return JSONObject().put("ok", reply.ok).put("answer", reply.answer).apply { if (!reply.ok) put("error", reply.answer) }
    }

    /** Journal the exchange and, if wanted, say the answer out loud on the phone. */
    private fun finish(source: String, mode: String, heard: String, reply: Brain.Reply) {
        Store.addJournal(this, source, mode, heard, reply.answer, !reply.ok)
        if (reply.ok && reply.speak.isNotBlank() && mode != "note" && Store.speakAnswers(this)) {
            Voice.speak(reply.speak, reply.speakLang)
        }
    }

    private fun findFaces(jpeg: ByteArray): JSONObject {
        val bitmap = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size) ?: return JSONObject().put("ok", false).put("faces", JSONArray())
        return try {
            val faces = Tasks.await(faceDetector.process(InputImage.fromBitmap(bitmap, 0)), 5, TimeUnit.SECONDS)
            val boxes = JSONArray()
            faces.forEach { boxes.put(JSONArray().put(it.boundingBox.left).put(it.boundingBox.top).put(it.boundingBox.width()).put(it.boundingBox.height())) }
            JSONObject().put("ok", true).put("faces", boxes)
        } finally {
            bitmap.recycle()
        }
    }

    private fun readQr(jpeg: ByteArray): JSONObject {
        val bitmap = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size) ?: return JSONObject().put("ok", false).put("text", "")
        return try {
            val code = Tasks.await(qrScanner.process(InputImage.fromBitmap(bitmap, 0)), 5, TimeUnit.SECONDS).firstOrNull()
            val points = JSONArray()
            code?.cornerPoints?.take(4)?.forEach { points.put(it.x).put(it.y) }
            JSONObject().put("ok", true).put("text", BoardText.ascii(code?.rawValue ?: "")).put("points", points)
        } finally {
            bitmap.recycle()
        }
    }

    private fun hunt(body: ByteArray): JSONObject {
        val request = JSONObject(String(body, Charsets.UTF_8))
        val reply = Brain.get(this).hunt(request.optString("theme"), request.optInt("count", 5))
        return if (reply.ok) JSONObject().put("ok", true).put("clues", JSONArray(reply.answer)) else error(reply.answer)
    }

    // ------------------------------------------------------------------ tiny HTTP helpers

    private fun error(message: String): JSONObject = JSONObject().put("ok", false).put("error", message).put("answer", message)

    private fun parseQuery(query: String): Map<String, String> = query.split('&').filter { '=' in it }.associate {
        URLDecoder.decode(it.substringBefore('='), "UTF-8") to URLDecoder.decode(it.substringAfter('='), "UTF-8")
    }

    private fun respond(output: BufferedOutputStream, status: Int, json: JSONObject) {
        val body = json.toString().toByteArray(Charsets.UTF_8)
        val reason = when (status) {
            200 -> "OK"; 400 -> "Bad Request"; 401 -> "Unauthorized"; 403 -> "Forbidden"; 404 -> "Not Found"
            413 -> "Payload Too Large"
            else -> "Internal Server Error"
        }
        output.write(
            ("HTTP/1.1 $status $reason\r\nContent-Type: application/json; charset=utf-8\r\n" +
                "Content-Length: ${body.size}\r\nConnection: close\r\n\r\n").toByteArray(Charsets.US_ASCII)
        )
        output.write(body)
        output.flush()
    }

    private fun respondBytes(output: BufferedOutputStream, type: String, body: ByteArray) {
        output.write(
            ("HTTP/1.1 200 OK\r\nContent-Type: $type\r\nContent-Length: ${body.size}\r\nConnection: close\r\n\r\n")
                .toByteArray(Charsets.US_ASCII)
        )
        output.write(body)
        output.flush()
    }

    private fun readLine(input: BufferedInputStream): String? {
        val bytes = ByteArrayOutputStream()
        while (true) {
            val next = input.read()
            if (next < 0) return if (bytes.size() == 0) null else bytes.toString("US-ASCII")
            if (next == '\n'.code) break
            if (next != '\r'.code) bytes.write(next)
            if (bytes.size() > 8192) throw IllegalArgumentException("Header line too long")
        }
        return bytes.toString("US-ASCII")
    }

    private fun readBody(input: BufferedInputStream, length: Int): ByteArray {
        val body = ByteArray(length)
        var offset = 0
        while (offset < length) {
            val count = input.read(body, offset, length - offset)
            if (count < 0) throw IllegalArgumentException("Request body ended early")
            offset += count
        }
        return body
    }
}

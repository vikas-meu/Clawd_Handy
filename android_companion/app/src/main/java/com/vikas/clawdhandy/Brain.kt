package com.vikas.clawdhandy

import android.content.Context
import android.util.Base64
import com.anthropic.client.AnthropicClient
import com.anthropic.client.okhttp.AnthropicOkHttpClient
import com.anthropic.core.JsonValue
import com.anthropic.errors.AnthropicIoException
import com.anthropic.errors.AnthropicServiceException
import com.anthropic.errors.RateLimitException
import com.anthropic.errors.UnauthorizedException
import com.anthropic.models.beta.messages.BetaBase64ImageSource
import com.anthropic.models.beta.messages.BetaContentBlockParam
import com.anthropic.models.beta.messages.BetaImageBlockParam
import com.anthropic.models.beta.messages.BetaJsonOutputFormat
import com.anthropic.models.beta.messages.BetaMessage
import com.anthropic.models.beta.messages.BetaMessageParam
import com.anthropic.models.beta.messages.BetaOutputConfig
import com.anthropic.models.beta.messages.BetaStopReason
import com.anthropic.models.beta.messages.BetaTool
import com.anthropic.models.beta.messages.BetaToolResultBlockParam
import com.anthropic.models.beta.messages.BetaToolUseBlock
import com.anthropic.models.beta.messages.BetaWebSearchTool20260209
import com.anthropic.models.beta.messages.MessageCreateParams
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Talks to Claude for the handheld and the app: questions (with web search and actions on the
 * phone and the handheld), photo questions, and translations. The API key never leaves the
 * phone except to go to Anthropic.
 */
class Brain private constructor(private val app: Context) {
    companion object {
        const val MODEL = "claude-opus-5-5"
        private const val FALLBACK_BETA = "server-side-fallback-2026-07-01"
        private const val MEMORY_MS = 15 * 60_000L // forget the conversation after 15 quiet minutes
        private const val MAX_TURNS = 6            // tool round-trips per question
        @Volatile private var instance: Brain? = null
        fun get(context: Context): Brain =
            instance ?: synchronized(this) { instance ?: Brain(context.applicationContext).also { instance = it } }

        /** The request for one turn. Pure, so it can be checked in a unit test. */
        internal fun params(
            system: String,
            messages: List<BetaMessageParam>,
            withTools: Boolean,
            format: BetaJsonOutputFormat? = null
        ): MessageCreateParams {
            val output = BetaOutputConfig.builder().effort(BetaOutputConfig.Effort.LOW) // quick answers for a handheld
            if (format != null) output.format(format)
            val params = MessageCreateParams.builder()
                .model(MODEL)
                .maxTokens(16000L)
                .system(system)
                .messages(messages)
                .outputConfig(output.build())
                // If a safety classifier declines, retry on a fallback model chosen by Anthropic
                .addBeta(FALLBACK_BETA)
                .putAdditionalBodyProperty("fallbacks", JsonValue.from("default"))
            if (withTools) {
                params.addTool(BetaWebSearchTool20260209.builder().maxUses(3L).build())
                TOOLS.forEach { params.addTool(it) }
            }
            return params.build()
        }

        /** JSON shape for translations: the real script for the phone to speak, ASCII for the handheld. */
        internal fun translationFormat(lang: String): BetaJsonOutputFormat {
            val schema = BetaJsonOutputFormat.Schema.builder()
                .putAdditionalProperty("type", JsonValue.from("object"))
                .putAdditionalProperty(
                    "properties", JsonValue.from(
                        mapOf(
                            "translation" to mapOf(
                                "type" to "string",
                                "description" to "The translation in $lang, in its normal script"
                            ),
                            "ascii" to mapOf(
                                "type" to "string",
                                "description" to "The same translation using only plain ASCII letters, for a tiny screen. " +
                                    "For non-Latin scripts give a readable romanized pronunciation; drop accents."
                            ),
                            "language_tag" to mapOf(
                                "type" to "string",
                                "description" to "BCP 47 tag of $lang for text-to-speech, e.g. hi-IN or es-ES"
                            )
                        )
                    )
                )
                .putAdditionalProperty("required", JsonValue.from(listOf("translation", "ascii", "language_tag")))
                .putAdditionalProperty("additionalProperties", JsonValue.from(false))
                .build()
            return BetaJsonOutputFormat.builder().schema(schema).build()
        }

        private fun tool(name: String, description: String, properties: Map<String, Any>, required: List<String>): BetaTool {
            val props = BetaTool.InputSchema.Properties.builder()
            properties.forEach { (key, schema) -> props.putAdditionalProperty(key, JsonValue.from(schema)) }
            return BetaTool.builder()
                .name(name)
                .description(description)
                .inputSchema(BetaTool.InputSchema.builder().properties(props.build()).required(required).build())
                .build()
        }

        internal val TOOLS: List<BetaTool> = listOf(
            tool(
                "set_handheld_alarm",
                "Set an alarm on the user's Clawd handheld. It rings with a tune and a dancing Clawd. Times are the user's local time.",
                mapOf(
                    "hour" to mapOf("type" to "integer", "minimum" to 0, "maximum" to 23, "description" to "Hour, 24-hour clock"),
                    "minute" to mapOf("type" to "integer", "minimum" to 0, "maximum" to 59),
                    "repeat" to mapOf("type" to "string", "enum" to listOf("once", "daily", "weekdays", "weekends"))
                ),
                listOf("hour", "minute", "repeat")
            ),
            tool(
                "start_focus_timer",
                "Start a Pomodoro focus timer on the handheld (a short break follows automatically).",
                mapOf("minutes" to mapOf("type" to "integer", "minimum" to 1, "maximum" to 120)),
                listOf("minutes")
            ),
            tool(
                "handheld_fun",
                "Make Clawd dance to chiptune music on the handheld, or feed the Clawd pet.",
                mapOf("action" to mapOf("type" to "string", "enum" to listOf("dance", "feed_pet"))),
                listOf("action")
            ),
            tool(
                "save_note",
                "Save a note to the user's notes in the phone app.",
                mapOf("text" to mapOf("type" to "string")),
                listOf("text")
            ),
            tool(
                "set_phone_reminder",
                "Remind the user with a phone notification after some minutes.",
                mapOf(
                    "text" to mapOf("type" to "string", "description" to "What to remind them about"),
                    "minutes_from_now" to mapOf("type" to "integer", "minimum" to 1, "maximum" to 10080)
                ),
                listOf("text", "minutes_from_now")
            ),
            tool(
                "control_phone_media",
                "Control whatever music or podcast is playing on the phone.",
                mapOf(
                    "action" to mapOf(
                        "type" to "string",
                        "enum" to listOf("play_pause", "next", "previous", "volume_up", "volume_down", "mute")
                    )
                ),
                listOf("action")
            ),
            tool(
                "open_on_phone",
                "Open something on the phone: turn-by-turn directions, a map search, play a song/artist/playlist, " +
                    "the dialer with a number, a web page, or a web search for the user to read.",
                mapOf(
                    "what" to mapOf(
                        "type" to "string",
                        "enum" to listOf("navigate", "map_search", "play_music", "dial", "open_url", "web_search")
                    ),
                    "query" to mapOf("type" to "string", "description" to "Place, song, phone number, URL or search terms")
                ),
                listOf("what", "query")
            )
        )
    }

    /** [answer] is what to show; [speak] (in [speakLang]) is what to say out loud on the phone. */
    data class Reply(
        val ok: Boolean,
        val answer: String,
        val speak: String = "",
        val speakLang: String = "",
        val actions: JSONArray = JSONArray()
    )

    private var client: AnthropicClient? = null
    private var clientKey = ""
    private val history = ArrayList<Pair<String, String>>() // (question, answer), oldest first
    private var lastTalkAt = 0L

    @Synchronized
    private fun client(): AnthropicClient? {
        val key = runCatching { PhoneSecrets.readApiKey(app) }.getOrDefault("")
        if (key.isBlank()) return null
        if (client == null || key != clientKey) {
            client = AnthropicOkHttpClient.builder().apiKey(key).build()
            clientKey = key
        }
        return client
    }

    fun forget() = synchronized(history) { history.clear() }

    // ------------------------------------------------------------------ entry points

    /** A spoken or typed request. mode: "ask", "translate" or "note"; source: "board" or "app". */
    fun chat(prompt: String, mode: String, lang: String, source: String): Reply {
        val text = prompt.trim()
        if (text.isEmpty()) return Reply(false, "Say or type something first.")
        if (mode == "note") {
            Store.addNote(app, text, 0)
            return Reply(true, "Saved to your notes on the phone.")
        }
        val board = source == "board"
        return guard {
            if (mode == "translate") translate(listOf(BetaContentBlockParam.ofText(translatePrompt(text, lang))), lang, board)
            else ask(text, board)
        }
    }

    /** A photo from the handheld. mode: "look" (what is it / solve it) or "translate". */
    fun photo(jpeg: ByteArray, mode: String, lang: String): Reply {
        if (jpeg.isEmpty()) return Reply(false, "No photo arrived.")
        val image = BetaContentBlockParam.ofImage(
            BetaImageBlockParam.builder().source(
                BetaBase64ImageSource.builder()
                    .mediaType(BetaBase64ImageSource.MediaType.IMAGE_JPEG)
                    .data(Base64.encodeToString(jpeg, Base64.NO_WRAP))
                    .build()
            ).build()
        )
        return guard {
            if (mode == "translate") {
                translate(
                    listOf(
                        image, BetaContentBlockParam.ofText(
                            "Translate the text in this photo (a sign, menu, label or page) into $lang. " +
                                "If there is no text, describe the photo in $lang in one sentence."
                        )
                    ), lang, board = true
                )
            } else {
                val response = send(
                    system(board = true),
                    listOf(
                        user(
                            image, BetaContentBlockParam.ofText(
                                "This photo is from my camera. If it shows a question, maths problem or puzzle, " +
                                    "solve it and give the short answer. Otherwise tell me briefly what you see."
                            )
                        )
                    ),
                    withTools = false
                )
                val answer = finalText(response)
                Reply(true, BoardText.ascii(answer).ifBlank { "Hmm, I have no answer for that." }, speak = answer)
            }
        }
    }

    // ------------------------------------------------------------------ ask (with tools)

    private fun ask(prompt: String, board: Boolean): Reply {
        val actions = JSONArray()
        val messages = ArrayList<BetaMessageParam>()
        synchronized(history) {
            if (System.currentTimeMillis() - lastTalkAt > MEMORY_MS) history.clear()
            for ((q, a) in history) {
                messages += BetaMessageParam.builder().role(BetaMessageParam.Role.USER).content(q).build()
                messages += BetaMessageParam.builder().role(BetaMessageParam.Role.ASSISTANT).content(a).build()
            }
        }
        messages += BetaMessageParam.builder().role(BetaMessageParam.Role.USER).content(prompt).build()
        val system = system(board)
        var answer: String? = null
        for (turn in 0 until MAX_TURNS) {
            val response = send(system, messages, withTools = true)
            when (response.stopReason().orElse(null)) {
                BetaStopReason.TOOL_USE -> {
                    messages += response.toParam()
                    val results = response.content().mapNotNull { it.toolUse().orElse(null) }
                        .map { runTool(it, board, actions) }
                    messages += BetaMessageParam.builder().role(BetaMessageParam.Role.USER)
                        .contentOfBetaContentBlockParams(results).build()
                }
                BetaStopReason.PAUSE_TURN -> messages += response.toParam() // a long web search: let it continue
                else -> {
                    answer = finalText(response)
                    break
                }
            }
        }
        val text = answer ?: "That took too many steps. Try asking more simply."
        synchronized(history) {
            history += prompt to text
            while (history.size > 6) history.removeAt(0)
            lastTalkAt = System.currentTimeMillis()
        }
        val shown = if (board) BoardText.ascii(text) else text
        return Reply(true, shown.ifBlank { "Done." }, speak = text, actions = actions)
    }

    // ------------------------------------------------------------------ translate (structured output)

    private fun translatePrompt(text: String, lang: String) =
        "Translate this into $lang. Keep the meaning and tone natural, as a native speaker would say it.\n\n$text"

    private fun translate(content: List<BetaContentBlockParam>, lang: String, board: Boolean): Reply {
        val response = send(
            "You are a precise, friendly translator.",
            listOf(user(*content.toTypedArray())),
            withTools = false,
            format = translationFormat(lang)
        )
        val json = runCatching { JSONObject(finalText(response)) }.getOrNull()
            ?: return Reply(false, "The translation came back garbled. Try again.")
        val native = json.optString("translation")
        val ascii = BoardText.ascii(json.optString("ascii"))
        val tag = json.optString("language_tag")
        val shown = if (board) ascii.ifBlank { BoardText.ascii(native) }
        else if (ascii.isNotBlank() && ascii != native) "$native\n($ascii)" else native
        return Reply(true, shown, speak = native, speakLang = tag)
    }

    // ------------------------------------------------------------------ the request

    private fun system(board: Boolean): String {
        val now = SimpleDateFormat("EEEE d MMMM yyyy, h:mm a zzzz", Locale.US).format(Date())
        val place = PhoneLocation.last(app)?.let { fix ->
            val city = if (fix.city.isNotBlank()) "${fix.city}, " else ""
            "The user is near ${city}latitude %.4f, longitude %.4f (from their phone's GPS).".format(Locale.US, fix.lat, fix.lon)
        } ?: "The user's location is unknown."
        val style = if (board) {
            "Your reply appears on a tiny 240x240 screen, so answer in at most two short sentences (under 40 words). " +
                "Plain text only: no markdown, lists, emoji or LaTeX. Give the answer first."
        } else {
            "Your reply appears in a phone app. Keep it short and conversational (under 120 words), plain text, no markdown."
        }
        return "You are Clawd, a pocket AI companion. You live in a small handheld camera gadget (an Adafruit " +
            "MEMENTO) and in the user's Android phone, which listens for you. $style\n\n" +
            "It is $now. $place\n\n" +
            "Use web search for anything current or local: news, weather, sports, prices, opening hours, places " +
            "near the user. When the user asks you to do something, use your tools: alarms, focus timers, dancing " +
            "and feeding the pet happen on the handheld; notes, reminders, music and opening apps happen on the " +
            "phone. After acting, confirm what you did in a few words."
    }

    private fun send(
        system: String,
        messages: List<BetaMessageParam>,
        withTools: Boolean,
        format: BetaJsonOutputFormat? = null
    ): BetaMessage {
        val client = client() ?: throw NoKey()
        val response = client.beta().messages().create(params(system, messages, withTools, format))
        Store.recordUsage(app, response.usage().inputTokens(), response.usage().outputTokens())
        if (response.stopReason().orElse(null) == BetaStopReason.REFUSAL) throw Refused()
        return response
    }

    private fun finalText(response: BetaMessage): String =
        response.content().mapNotNull { it.text().orElse(null)?.text() }.joinToString(" ").trim()

    private fun user(vararg blocks: BetaContentBlockParam): BetaMessageParam =
        BetaMessageParam.builder().role(BetaMessageParam.Role.USER).contentOfBetaContentBlockParams(blocks.toList()).build()

    private class NoKey : Exception()
    private class Refused : Exception()

    /** Turns failures into a short message for the screen. */
    private fun guard(work: () -> Reply): Reply = try {
        work()
    } catch (_: NoKey) {
        Reply(false, "Add your Anthropic API key in the Clawd Phone app.")
    } catch (_: Refused) {
        Reply(false, "Sorry, I can't help with that one.")
    } catch (_: UnauthorizedException) {
        Reply(false, "Anthropic rejected the API key. Check it in the phone app.")
    } catch (_: RateLimitException) {
        Reply(false, "Rate limited. Try again in a minute.")
    } catch (e: AnthropicServiceException) {
        Reply(false, "Claude API error ${e.statusCode()}. Try again.")
    } catch (_: AnthropicIoException) {
        Reply(false, "The phone can't reach Claude. Check its internet.")
    } catch (e: Exception) {
        Reply(false, "Something went wrong: ${e.javaClass.simpleName}")
    }

    // ------------------------------------------------------------------ tools

    /** Runs one tool call. Handheld actions go back to the board in [actions] (or wait for its next poll). */
    private fun runTool(call: BetaToolUseBlock, board: Boolean, actions: JSONArray): BetaContentBlockParam {
        val input = runCatching { JSONObject(call.input(Map::class.java) as Map<*, *>) }.getOrDefault(JSONObject())
        fun handheld(action: JSONObject, done: String): String {
            if (board) actions.put(action) else Store.queueBoardAction(app, action)
            return if (board) done else "$done (the handheld picks this up within a minute)"
        }
        val (result, isError) = try {
            when (call.name()) {
                "set_handheld_alarm" -> {
                    val hour = input.getInt("hour")
                    val minute = input.getInt("minute")
                    require(hour in 0..23 && minute in 0..59) { "Time out of range" }
                    val repeat = listOf("once", "daily", "weekdays", "weekends").indexOf(input.optString("repeat")).coerceAtLeast(0)
                    handheld(
                        JSONObject().put("type", "alarm").put("hour", hour).put("minute", minute).put("repeat", repeat),
                        "Alarm set for %d:%02d".format(hour, minute)
                    ) to false
                }
                "start_focus_timer" -> {
                    val minutes = input.getInt("minutes").coerceIn(1, 120)
                    handheld(JSONObject().put("type", "focus").put("minutes", minutes), "Focus timer started for $minutes min") to false
                }
                "handheld_fun" -> {
                    val feed = input.optString("action") == "feed_pet"
                    handheld(JSONObject().put("type", if (feed) "feed" else "dance"), if (feed) "Fed Clawd" else "Clawd is dancing") to false
                }
                "save_note" -> {
                    Store.addNote(app, input.getString("text"), 0)
                    "Note saved" to false
                }
                "set_phone_reminder" ->
                    PhoneActions.remind(app, input.getString("text"), input.getInt("minutes_from_now")) to false
                "control_phone_media" -> PhoneActions.media(app, input.getString("action")) to false
                "open_on_phone" -> PhoneActions.open(app, input.getString("what"), input.getString("query")) to false
                else -> "Unknown tool ${call.name()}" to true
            }
        } catch (e: Exception) {
            "Couldn't do that: ${e.message ?: e.javaClass.simpleName}" to true
        }
        return BetaContentBlockParam.ofToolResult(
            BetaToolResultBlockParam.builder().toolUseId(call.id()).content(result).isError(isError).build()
        )
    }
}

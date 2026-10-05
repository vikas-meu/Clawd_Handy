package com.vikas.clawdhandy

import com.anthropic.core.jsonMapper
import com.anthropic.models.beta.messages.BetaMessageParam
import com.fasterxml.jackson.databind.JsonNode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** Checks the JSON the phone sends to the Claude API, without calling it. */
class BrainRequestTest {
    private fun body(withTools: Boolean, translateTo: String? = null): JsonNode {
        val messages = listOf(BetaMessageParam.builder().role(BetaMessageParam.Role.USER).content("hi").build())
        val format = translateTo?.let { Brain.translationFormat(it) }
        val params = Brain.params("system prompt", messages, withTools, format)
        val json = jsonMapper().writeValueAsString(params._body())
        println(json)
        return jsonMapper().readTree(json)
    }

    @Test
    fun askRequestHasModelEffortFallbacksAndTools() {
        val body = body(withTools = true)
        assertEquals("claude-opus-5-5", body["model"].asText())
        assertEquals("low", body["output_config"]["effort"].asText())
        assertEquals("default", body["fallbacks"].asText())
        assertTrue("no thinking override", body["thinking"] == null)
        val tools = body["tools"]
        assertEquals("web_search_20260209", tools[0]["type"].asText())
        val names = (1 until tools.size()).map { tools[it]["name"].asText() }
        assertEquals(
            listOf("set_handheld_alarm", "start_focus_timer", "handheld_fun", "save_note", "set_phone_reminder",
                "control_phone_media", "open_on_phone"),
            names
        )
        for (i in 1 until tools.size()) {
            val schema = tools[i]["input_schema"]
            assertEquals("object", schema["type"].asText())
            assertTrue(schema["required"].size() > 0)
        }
        assertEquals(listOf("once", "daily", "weekdays", "weekends"), tools[1]["input_schema"]["properties"]["repeat"]["enum"].map { it.asText() })
    }

    @Test
    fun translateRequestUsesJsonSchemaWithoutTools() {
        val body = body(withTools = false, translateTo = "Hindi")
        assertTrue(body["tools"] == null)
        val format = body["output_config"]["format"]
        assertEquals("json_schema", format["type"].asText())
        assertEquals(listOf("translation", "ascii", "language_tag"), format["schema"]["required"].map { it.asText() })
        assertEquals(false, format["schema"]["additionalProperties"].asBoolean())
    }
}

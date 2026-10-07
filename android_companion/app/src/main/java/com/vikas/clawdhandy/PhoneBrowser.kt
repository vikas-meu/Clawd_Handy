package com.vikas.clawdhandy

import org.json.JSONArray
import org.json.JSONObject
import org.jsoup.Jsoup
import org.jsoup.nodes.Document
import java.net.URLDecoder
import java.net.URLEncoder

/**
 * The handheld's web browser, Lynx-style. A full browser can't run on a microcontroller, so
 * the phone fetches the page, keeps the readable text and the links, and the board shows those
 * on its 240x240 screen. Anything that isn't a web address becomes a DuckDuckGo search.
 */
object PhoneBrowser {
    private const val UA = "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 (KHTML, like Gecko) " +
        "Chrome/124.0 Mobile Safari/537.36"
    private const val MAX_CHARS = 7000
    private const val MAX_LINKS = 40
    private val DOMAIN = Regex("""^[\w-]+(\.[\w-]+)+(/\S*)?$""")

    /** [input] is a URL, a domain ("youtube.com"), something spoken ("open wikipedia dot org") or search words. */
    fun open(input: String): JSONObject {
        var text = input.trim().replace(Regex("""\s+dot\s+""", RegexOption.IGNORE_CASE), ".")
        text = text.replace(Regex("""^(open|go to|visit|search for|search)\s+""", RegexOption.IGNORE_CASE), "").trim()
        if (text.isEmpty()) return error("Say or type a web address or something to search for")
        val url = when {
            text.startsWith("http://") || text.startsWith("https://") -> text
            DOMAIN.matches(text.lowercase()) -> "https://${text.lowercase()}"
            else -> "https://html.duckduckgo.com/html/?q=" + URLEncoder.encode(text, "UTF-8")
        }
        return try {
            val doc = Jsoup.connect(url).userAgent(UA).timeout(15_000).followRedirects(true).maxBodySize(3_000_000).get()
            if (doc.location().contains("duckduckgo.com/html")) searchResults(doc, text) else page(doc)
        } catch (e: Exception) {
            error("Couldn't open that page: ${e.javaClass.simpleName}")
        }
    }

    private fun page(doc: Document): JSONObject {
        doc.select("script, style, noscript, svg, nav, footer, header, aside, form, iframe, button").remove()
        val root = doc.selectFirst("article") ?: doc.selectFirst("main") ?: doc.body()
        val blocks = JSONArray()
        var chars = 0
        for (el in root.select("h1, h2, h3, p, pre, blockquote, li:not(:has(p))")) {
            val text = BoardText.ascii(el.text()).trim()
            if (text.length < 2) continue
            val heading = el.tagName() in setOf("h1", "h2", "h3")
            blocks.put(JSONObject().put("t", if (heading) "h" else "p").put("x", text.take(600)))
            chars += text.length
            if (chars > MAX_CHARS || blocks.length() >= 80) break
        }
        if (blocks.length() == 0) blocks.put(JSONObject().put("t", "p").put("x", "This page has no readable text (it may need JavaScript)."))
        val links = JSONArray()
        val seen = HashSet<String>()
        for (a in root.select("a[href]")) {
            val href = a.absUrl("href")
            val label = BoardText.ascii(a.text()).trim()
            if (!href.startsWith("http") || label.length < 2 || !seen.add(href)) continue
            links.put(JSONObject().put("text", label.take(60)).put("url", href))
            if (links.length() >= MAX_LINKS) break
        }
        return JSONObject().put("ok", true).put("title", BoardText.ascii(doc.title()).take(80)).put("url", doc.location())
            .put("blocks", blocks).put("links", links)
    }

    private fun searchResults(doc: Document, query: String): JSONObject {
        val blocks = JSONArray()
        val links = JSONArray()
        for (result in doc.select(".result").take(12)) {
            val a = result.selectFirst("a.result__a") ?: continue
            var href = a.absUrl("href")
            // DuckDuckGo wraps results in a redirect: //duckduckgo.com/l/?uddg=<real url>
            Regex("""[?&]uddg=([^&]+)""").find(href)?.let { href = URLDecoder.decode(it.groupValues[1], "UTF-8") }
            if (!href.startsWith("http")) continue
            val title = BoardText.ascii(a.text()).trim()
            val snippet = BoardText.ascii(result.selectFirst(".result__snippet")?.text() ?: "").trim()
            blocks.put(JSONObject().put("t", "h").put("x", title.take(120)))
            if (snippet.isNotEmpty()) blocks.put(JSONObject().put("t", "p").put("x", snippet.take(300)))
            links.put(JSONObject().put("text", title.take(60)).put("url", href))
        }
        if (links.length() == 0) blocks.put(JSONObject().put("t", "p").put("x", "No results."))
        return JSONObject().put("ok", true).put("title", BoardText.ascii("Search: $query").take(80))
            .put("url", doc.location()).put("blocks", blocks).put("links", links)
    }

    private fun error(message: String) = JSONObject().put("ok", false).put("error", message)
}

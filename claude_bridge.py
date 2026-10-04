"""
Claude Desk Buddy bridge - runs on your PC and serves the MEMENTO board over your LAN.

  GET  /usage[?fresh=1]          -> plan usage (5-hour session + weekly)
  POST /ask/image  body = JPEG   -> Claude looks at the photo, short answer
  POST /face       body = JPEG   -> face boxes, found locally with OpenCV
  POST /qr         body = JPEG   -> text of a QR code in the frame, read locally with OpenCV

Setup:
  pip install -r requirements.txt
  set ANTHROPIC_API_KEY=sk-ant-...          (Claude API key - billed separately from your plan)
  python claude_bridge.py
"""
import base64
import json
import os
import pathlib
import re
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import anthropic

PORT = 8765
MODEL = "claude-opus-5-5"
EFFORT = "low"            # low effort keeps thinking (and token spend) small
USAGE_CACHE_S = 60
USAGE_MIN_REFRESH_S = 15

CREDS = pathlib.Path.home() / ".claude" / ".credentials.json"

SYSTEM = (
    "You are Claude, living inside a tiny handheld gadget with a 240x240 pixel screen. "
    "Your reply is shown on that small screen, so answer in at most two short sentences "
    "(under 35 words). Plain ASCII text only: no markdown, lists, emojis or LaTeX. "
    "For maths or reasoning problems, give the final answer first, then a few words on why."
)
PHOTO_PROMPT = (
    "This photo is from my camera. If it shows a question, maths problem or puzzle, "
    "solve it and give the short answer. Otherwise tell me briefly what you see."
)

client = anthropic.Anthropic()  # reads ANTHROPIC_API_KEY
_lock = threading.Lock()
_usage_cache = {"t": 0.0, "data": None}


# ---------------------------------------------------------------- usage
def _fmt_duration(iso: str) -> str:
    secs = (datetime.fromisoformat(iso) - datetime.now(timezone.utc)).total_seconds()
    if secs <= 0:
        return "now"
    mins = int(secs // 60)
    days, hours, mins = mins // 1440, (mins % 1440) // 60, mins % 60
    if days:
        return f"{days}d {hours}h"
    if hours:
        return f"{hours}h {mins}m"
    return f"{mins}m"


def fetch_usage() -> dict:
    """Reads the plan usage Claude Code shows in /usage. This endpoint is undocumented
    and may change. The token comes from Claude Code's login and is never sent to the board."""
    try:
        oauth = json.loads(CREDS.read_text())["claudeAiOauth"]
    except (OSError, KeyError, ValueError):
        return {"ok": False, "error": "Log in to Claude Code on this PC"}
    req = urllib.request.Request(
        "https://api.anthropic.com/api/oauth/usage",
        headers={
            "Authorization": "Bearer " + oauth["accessToken"],
            "anthropic-beta": "oauth-2025-04-20",
            "User-Agent": "claude-desk-buddy/1.0",
        },
    )
    try:
        raw = json.load(urllib.request.urlopen(req, timeout=10))
    except urllib.error.HTTPError as e:
        if e.code == 401:
            return {"ok": False, "error": "Login expired - open Claude Code"}
        return {"ok": False, "error": f"Usage API error {e.code}"}
    except (urllib.error.URLError, TimeoutError):
        return {"ok": False, "error": "PC has no internet"}

    def window(key):
        w = raw.get(key) or {}
        return {
            "pct": round(float(w.get("utilization") or 0), 1),
            "resets": _fmt_duration(w["resets_at"]) if w.get("resets_at") else "",
        }

    rows = (raw.get("seven_day_breakdown") or {}).get("rows") or []
    return {
        "ok": True,
        "plan": (oauth.get("subscriptionType") or "").upper()[:5],
        "session": window("five_hour"),
        "weekly": window("seven_day"),
        "breakdown": [{"name": ascii_only(r["display_name"])[:14], "pct": r["percent"]} for r in rows[:4]],
    }


def get_usage(fresh: bool) -> dict:
    with _lock:
        age = time.time() - _usage_cache["t"]
        if _usage_cache["data"] and (age < USAGE_CACHE_S and not (fresh and age > USAGE_MIN_REFRESH_S)):
            return _usage_cache["data"]
    data = fetch_usage()
    with _lock:
        _usage_cache.update(t=time.time(), data=data)
    return data


# ---------------------------------------------------------------- claude
_REPLACEMENTS = {
    "×": "x", "÷": "/", "−": "-", "–": "-", "—": "-", "‘": "'",
    "’": "'", "“": '"', "”": '"', "…": "...", "√": "sqrt", "π": "pi",
    "²": "^2", "³": "^3", "°": " deg", "≈": "~", "≠": "!=", "≤": "<=",
    "≥": ">=", "→": "->", "½": "1/2", "¼": "1/4", "¾": "3/4",
}


def ascii_only(text: str) -> str:
    """The board's built-in font only has ASCII, so swap common symbols and drop the rest."""
    for k, v in _REPLACEMENTS.items():
        text = text.replace(k, v)
    text = re.sub(r"[*_`#]+", "", text)  # stray markdown
    text = text.encode("ascii", "ignore").decode()
    return re.sub(r"\s+", " ", text).strip()


def ask_claude(content: list) -> str:
    try:
        response = client.beta.messages.create(
            model=MODEL,
            max_tokens=2000,  # includes thinking; the system prompt keeps the answer short
            system=SYSTEM,
            output_config={"effort": EFFORT},
            betas=["server-side-fallback-2026-07-01"],
            extra_body={"fallbacks": "default"},  # retry on a fallback model if declined
            messages=[{"role": "user", "content": content}],
        )
    except anthropic.AuthenticationError:
        return "ERROR: API key missing or invalid on the PC"
    except anthropic.RateLimitError:
        return "ERROR: Rate limited, try again in a minute"
    except anthropic.APIStatusError as e:
        return f"ERROR: Claude API error {e.status_code}"
    except anthropic.APIConnectionError:
        return "ERROR: PC can't reach the Claude API"
    except TypeError:  # the SDK found no credentials at all
        return "ERROR: Set ANTHROPIC_API_KEY on the PC"

    if response.stop_reason == "refusal":
        return "Sorry, I can't help with that one."
    text = " ".join(b.text for b in response.content if b.type == "text")
    u = response.usage
    print(f"  tokens: in={u.input_tokens} out={u.output_tokens}")
    return ascii_only(text) or "Hmm, I have no answer for that."


def image_block(jpeg: bytes) -> dict:
    return {
        "type": "image",
        "source": {"type": "base64", "media_type": "image/jpeg", "data": base64.standard_b64encode(jpeg).decode()},
    }


# ---------------------------------------------------------------- face finder
_face_detector = None


def find_faces(jpeg: bytes) -> dict:
    """Face boxes for one small camera frame. Runs locally with OpenCV, no API involved."""
    global _face_detector
    try:
        import cv2
        import numpy as np
    except ImportError:
        return {"ok": False, "faces": [], "error": "pip install opencv-python-headless"}
    if _face_detector is None:
        _face_detector = cv2.CascadeClassifier(cv2.data.haarcascades + "haarcascade_frontalface_default.xml")
    img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_GRAYSCALE)
    if img is None:
        return {"ok": False, "faces": []}
    img = cv2.equalizeHist(img)  # helps in dim rooms
    with _lock:  # the detector isn't thread-safe
        found = _face_detector.detectMultiScale(img, scaleFactor=1.15, minNeighbors=5, minSize=(24, 24))
    return {"ok": True, "faces": [[int(v) for v in f] for f in found]}


_qr_reader = None


def read_qr(jpeg: bytes) -> dict:
    """Text of a QR code in one camera frame, plus its 4 corners for the board to outline."""
    global _qr_reader
    try:
        import cv2
        import numpy as np
    except ImportError:
        return {"ok": False, "text": "", "error": "pip install opencv-python-headless"}
    if _qr_reader is None:
        _qr_reader = cv2.QRCodeDetector()
    img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_GRAYSCALE)
    if img is None:
        return {"ok": False, "text": ""}
    with _lock:
        text, points, _ = _qr_reader.detectAndDecode(img)
    if not text or points is None:
        return {"ok": True, "text": ""}
    print(f"QR code: {text}")
    return {"ok": True, "text": ascii_only(text), "points": [int(v) for v in points.reshape(-1)[:8]]}


# ---------------------------------------------------------------- http
class Handler(BaseHTTPRequestHandler):
    def _send(self, obj: dict):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _query(self):
        url = urllib.parse.urlparse(self.path)
        return url.path, dict(urllib.parse.parse_qsl(url.query))

    def do_GET(self):
        path, q = self._query()
        if path == "/usage":
            self._send(get_usage(q.get("fresh") == "1"))
        else:
            self.send_error(404)

    def do_POST(self):
        try:
            self._post()
        except Exception as e:  # always answer, or the board waits for its timeout
            print(f"  failed: {e!r}")
            self._send(_result("ERROR: Bridge crashed - see its window"))

    def _post(self):
        path, q = self._query()
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))

        if path == "/ask/image":
            print("Photo question...")
            answer = ask_claude([image_block(body), {"type": "text", "text": PHOTO_PROMPT}])
            print(f"  answer: {answer}")
            self._send(_result(answer))

        elif path == "/face":
            self._send(find_faces(body))

        elif path == "/qr":
            self._send(read_qr(body))

        else:
            self.send_error(404)

    def log_message(self, fmt, *args):  # keep the console readable
        pass


def _result(answer: str) -> dict:
    return {"ok": not answer.startswith("ERROR:"), "answer": answer.removeprefix("ERROR: ")}


DISCOVERY_PORT = 8766


def discovery_responder():
    """Answers the board's LAN broadcast so it finds this PC even after its IP changes."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", DISCOVERY_PORT))
    while True:
        data, addr = s.recvfrom(64)
        if data.strip() == b"CLAUDE_BRIDGE?":
            print(f"Board at {addr[0]} found the bridge")
            s.sendto(f"CLAUDE_BRIDGE {PORT}".encode(), addr)


def lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


if __name__ == "__main__":
    if not os.environ.get("ANTHROPIC_API_KEY"):
        print("WARNING: ANTHROPIC_API_KEY is not set - Claude Ask will not work (usage still will).")
    threading.Thread(target=discovery_responder, daemon=True).start()
    print(f'Bridge running. In the sketch set:  #define BRIDGE_URL "http://{lan_ip()}:{PORT}"')
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()

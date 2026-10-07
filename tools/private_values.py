"""
Keeps your WiFi password and tokens out of git while they live right in the sketch.

Lines like this in claude_desk_buddy.ino are "private":

    #define WIFI_PASSWORD "hunter2" // private

  clean  (git add / commit):  the value goes to git as its placeholder, e.g. "your-wifi-password",
                              and your real value is remembered in this clone's .git/config
  smudge (git checkout/pull): placeholders are swapped back for your remembered values

So your working copy keeps the real values and GitHub only ever sees placeholders.
Set up once per clone (from the repo root):

    git config filter.private-values.clean  "python tools/private_values.py clean"
    git config filter.private-values.smudge "python tools/private_values.py smudge"
    git config filter.private-values.required true
"""
import re
import subprocess
import sys

PLACEHOLDERS = {
    "WIFI_SSID": "your-wifi-name",
    "WIFI_PASSWORD": "your-wifi-password",
    "BOTtoken": "123456789:your-telegram-bot-token",
    "CHAT_ID": "your-telegram-chat-id",
    "YT_HANDLE": "@yourchannel",
    "YT_API_KEY": "your-youtube-api-key",
}
LINE = re.compile(r'^(#define (\w+) )"((?:[^"\\\r\n]|\\.)*)"( *// private[^\r\n]*)(?=\r?\n|\Z)', re.M)


def git_config(*args):
    return subprocess.run(["git", "config", "--local", *args], capture_output=True, text=True).stdout.strip()


def clean(text):
    def hide(m):
        name, value = m.group(2), m.group(3)
        placeholder = PLACEHOLDERS.get(name, "")
        if value != placeholder and value != git_config("--get", f"clawd-private.{name}.value"):
            git_config(f"clawd-private.{name}.value", value)  # remember it for smudge
        return f'{m.group(1)}"{placeholder}"{m.group(4)}'

    return LINE.sub(hide, text)


def smudge(text):
    def restore(m):
        name, value = m.group(2), m.group(3)
        saved = git_config("--get", f"clawd-private.{name}.value")
        if saved and value == PLACEHOLDERS.get(name, ""):
            value = saved
        return f'{m.group(1)}"{value}"{m.group(4)}'

    return LINE.sub(restore, text)


if __name__ == "__main__":
    data = sys.stdin.buffer.read().decode("utf-8")
    out = clean(data) if sys.argv[1:] == ["clean"] else smudge(data)
    sys.stdout.buffer.write(out.encode("utf-8"))

"""
Renders the README images (boot.gif, dance.gif, screens.png) by re-drawing the board's
screens in Python: same shapes, colours and 5x7 pixel font as the sketch, at 240x240.

    pip install pillow
    python docs/make_images.py

The font comes from the Adafruit GFX library in your Arduino libraries folder.
"""
import math
import pathlib
import re

from PIL import Image, ImageDraw

OUT = pathlib.Path(__file__).parent
FONT_C = pathlib.Path.home() / "Documents/Arduino/libraries/Adafruit_GFX_Library/glcdfont.c"
FONT = [int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", FONT_C.read_text())]
SCALE = 2  # 240 px screens become 480 px images, with crisp pixels

ORANGE, DARK, CREAM = (217, 119, 87), (20, 20, 19), (250, 249, 245)
GRAY, TRACK, AMBER = (138, 138, 138), (46, 45, 42), (232, 162, 58)
RED, GREEN, BLUE = (229, 72, 77), (76, 175, 80), (91, 163, 248)


class Screen:
    def __init__(self):
        self.img = Image.new("RGB", (240, 240), DARK)
        self.d = ImageDraw.Draw(self.img)

    def fill(self, c):
        self.d.rectangle([0, 0, 239, 239], fill=c)

    def rect(self, x, y, w, h, c):
        if w > 0 and h > 0:
            self.d.rectangle([x, y, x + w - 1, y + h - 1], fill=c)

    def rrect(self, x, y, w, h, r, c, outline=False):
        box = [x, y, x + w - 1, y + h - 1]
        if outline:
            self.d.rounded_rectangle(box, r, outline=c)
        else:
            self.d.rounded_rectangle(box, r, fill=c)

    def circle(self, x, y, r, c, outline=False):
        box = [x - r, y - r, x + r, y + r]
        self.d.ellipse(box, outline=c) if outline else self.d.ellipse(box, fill=c)

    def tri(self, pts, c):
        self.d.polygon(pts, fill=c)

    def line(self, x0, y0, x1, y1, c):
        self.d.line([x0, y0, x1, y1], fill=c)

    def text(self, x, y, s, size, c):
        """Adafruit GFX classic font: 5x7 glyphs in a 6x8 cell, scaled by size."""
        for ch in s:
            o = ord(ch) * 5
            for col in range(5):
                bits = FONT[o + col]
                for row in range(8):
                    if bits & (1 << row):
                        self.rect(x + col * size, y + row * size, size, size, c)
            x += 6 * size

    def centered(self, s, y, size, c):
        self.text(120 - len(s) * 3 * size, y, s, size, c)

    def out(self):
        return self.img.resize((240 * SCALE, 240 * SCALE), Image.NEAREST)


LENS = [1.00, 0.80, 0.94, 0.76, 1.00, 0.84, 0.90, 0.78, 1.00, 0.82, 0.96, 0.77]
JITTER = [0, 4, -3, 2, -2, 5, 0, -4, 3, -1, 2, -3]


def logo(s, cx, cy, r, rot, c):
    """The 12-ray Claude spark, as drawn by drawClaudeLogo()."""
    if r < 2:
        return
    hw = r * 0.085
    for i in range(12):
        a = math.radians(i * 30 + JITTER[i] + rot)
        ca, sa = math.cos(a), math.sin(a)
        px, py = -sa, ca
        bw, tw = hw * 1.15, hw * 0.85
        tx, ty = cx + ca * r * LENS[i], cy + sa * r * LENS[i]
        s.tri([(cx + px * bw, cy + py * bw), (cx - px * bw, cy - py * bw),
               (tx - px * tw, ty - py * tw), (tx + px * tw, ty + py * tw)], c)
        s.circle(tx, ty, max(1, tw), c)
    s.circle(cx, cy, r * 0.2, c)


def bot(s, x, y, U, face="normal", right=0, left=0):
    """Clawd, as drawn by drawBot(): 7x4 block body, eyes, arms (0 down, 1 out, 2 up), legs."""
    s.rect(x, y, 7 * U, 4 * U, ORANGE)
    shift = {"left": -U // 3, "right": U // 3}.get(face, 0)
    thick = max(2, U // 4)
    for ex0 in (U, 5 * U):
        ex, ey = x + ex0 + shift, y + U * 3 // 4
        if face == "blink":
            s.rect(ex, ey + U * 3 // 4, U, thick, DARK)
        elif face == "happy":
            s.tri([(ex, ey + U), (ex + U / 2, ey + U / 4), (ex + U, ey + U)], DARK)
            s.tri([(ex + thick, ey + U + 1), (ex + U / 2, ey + U / 4 + thick), (ex + U - thick, ey + U + 1)], ORANGE)
        elif face == "worried":
            s.rect(ex, ey + U // 3, U, U, DARK)
        elif face == "dead":
            for t in range(thick):
                s.line(ex + t, ey, ex + U - thick + t, ey + U, DARK)
                s.line(ex + U - 1 - t, ey, ex + thick - 1 - t, ey + U, DARK)
        else:
            s.rect(ex, ey, U, U * 3 // 2, DARK)
    for side, pose in ((-1, left), (1, right)):
        ax = x - U if side < 0 else x + 7 * U
        if pose == 0:
            s.rect(ax, y + 2 * U, U, U, ORANGE)
        elif pose == 1:
            s.rect(ax, y + U, U, U, ORANGE)
        else:
            s.rect(ax, y, U, U, ORANGE)
            s.rect(ax + side * U, y - U, U, U, ORANGE)
    for lx in (U, 2 * U, U * 9 // 2, U * 11 // 2):
        s.rect(x + lx, y + 4 * U, max(2, U // 2), U, ORANGE)


def bubble(s, x, y, w, h, tx, ty):
    s.rrect(x, y, w, h, 10, CREAM)
    if ty >= y + h:
        bx = max(x + 12, min(tx, x + w - 26))
        s.tri([(bx, y + h - 1), (bx + 14, y + h - 1), (tx, ty)], CREAM)
    elif ty < y:
        bx = max(x + 12, min(tx, x + w - 26))
        s.tri([(bx, y), (bx + 14, y), (tx, ty)], CREAM)
    else:
        by = max(y + 8, min(ty, y + h - 20))
        s.tri([(x, by), (x, by + 12), (tx, ty)], CREAM)


def ease_out_back(t):
    c1 = 1.70158
    return 1 + (c1 + 1) * (t - 1) ** 3 + c1 * (t - 1) ** 2


def save_gif(frames, name, ms):
    frames[0].save(OUT / name, save_all=True, append_images=frames[1:], duration=ms, loop=0, optimize=True)
    print(f"{name}: {len(frames)} frames, {(OUT / name).stat().st_size // 1024} KB")


# ---------------------------------------------------------------- boot.gif
def boot_gif():
    frames, durs = [], []

    def add(s, ms):
        frames.append(s.out())
        durs.append(ms)

    for f in range(29):  # spark pops and spins in
        t = f / 28
        s = Screen()
        logo(s, 120, 100, 60 * ease_out_back(t), -120 * (1 - t), ORANGE)
        add(s, 30)
    for n in range(1, 7):  # "Claude" types out
        s = Screen()
        logo(s, 120, 100, 60, 0, ORANGE)
        s.text(66, 180, "Claude"[:n], 3, CREAM)
        add(s, 90 if n < 6 else 900)
    for f in range(12, -1, -1):  # spin + shrink out
        s = Screen()
        logo(s, 120, 100, 60 * f / 12, 180 * (12 - f) / 12, ORANGE)
        add(s, 30)
    bx = (240 - 7 * 14) // 2
    for f in range(21):  # Clawd jumps up
        y = 250 + int((120 - 250) * ease_out_back(f / 20))
        s = Screen()
        bot(s, bx, y, 14)
        add(s, 30)

    def hi_scene(face, arm, chars, ms):
        s = Screen()
        bot(s, bx, 120, 14, face, arm)
        if chars:
            bubble(s, 120, 30, 105, 52, 140, 100)
            s.text(145, 44, "Hi!"[:chars], 3, ORANGE)
        add(s, ms)

    hi_scene("normal", 0, 0, 200)
    for n in range(1, 4):
        hi_scene("happy", 1, n, 120)
    for w in range(3):
        hi_scene("blink" if w == 1 else "happy", 2, 3, 160)
        hi_scene("happy", 1, 3, 160)
    hi_scene("normal", 0, 3, 1500)
    frames[0].save(OUT / "boot.gif", save_all=True, append_images=frames[1:], duration=durs, loop=0, optimize=True)
    print(f"boot.gif: {len(frames)} frames, {(OUT / 'boot.gif').stat().st_size // 1024} KB")


# ---------------------------------------------------------------- dance.gif
DRUMS = "K.H.S.HKH.K.S.HO"
MOVES = ["Bounce!", "Side step!", "Arm wave!", "Robot pop!"]


def dance_pose(step, phase, move):
    beat_num, beat = step // 4, ((step % 4) + phase) / 4
    dx = dy = left = right = 0
    face = "happy"
    if move == 0:
        dy = -int(12 * math.sin(beat * math.pi))
        left, right = (2, 0) if beat_num % 2 else (0, 2)
    elif move == 1:
        sway = math.sin((beat_num % 2 + beat) * math.pi)
        dx, dy = int(20 * sway), int(4 * abs(sway))
        face = "right" if sway > 0.2 else "left" if sway < -0.2 else "happy"
        left = right = 1
    elif move == 2:
        left, right = (2, 1) if (step // 2) % 2 else (1, 2)
        dy = -int(4 * abs(math.sin(beat * math.pi)))
        face = "blink" if DRUMS[step % 16] == "S" else "normal"
    else:
        poses = [(-10, 0, 2, 0), (10, 4, 0, 2), (0, -8, 2, 2), (0, 6, 1, 1)]
        dx, dy, left, right = poses[(step // 2) % 4]
        face = "blink" if (step // 2) % 4 == 2 else "normal"
    return dx, dy, left, right, face


def note(s, x, y, c):
    s.circle(x, y, 4, c)
    s.rect(x + 3, y - 14, 1, 14, c)
    s.line(x + 3, y - 14, x + 9, y - 10, c)
    s.line(x + 4, y - 14, x + 10, y - 10, c)


def dance_gif():
    frames, step_ms = [], 15000 // 92
    for step in range(64):  # one bar of each move
        for sub in range(2):
            phase, now = sub / 2, (step * 2 + sub) * step_ms // 2
            move = step // 16
            dx, dy, left, right, face = dance_pose(step, phase, move)
            x, y = (240 - 98) // 2 + dx, 104 + dy
            kick, snare = DRUMS[step % 16] == "K", DRUMS[step % 16] == "S"
            s = Screen()
            logo(s, 120, 32, 18 + (4 * (1 - phase) if kick else 0), step * 15 + phase * 15, ORANGE)
            if kick and phase < 0.8:
                s.circle(120, 32, 24 + int(phase * 40), ORANGE, outline=True)
            if snare and phase < 0.8:
                s.circle(120, 32, 24 + int(phase * 30), CREAM, outline=True)
            for i in range(4):
                nx = (216 if i % 2 else 12) + int(5 * math.sin(now / 300 + i))
                note(s, nx, 180 - (now // 25 + i * 45) % 150, ORANGE if i % 2 else CREAM)
            sw = 90 - max(0, 104 - y) * 3
            s.rrect(x + 49 - sw // 2, 178, sw, 6, 3, TRACK)
            bot(s, x, y, 14, face, right, left)
            for i in range(8):
                c = ORANGE if i == step % 8 else TRACK if (i + step // 4) % 2 else (28, 28, 28)
                s.rect(i * 30 + 1, 188, 28, 10, c)
            s.text(4, 4, "Boom bap", 1, GRAY)
            s.centered(MOVES[move], 205, 2, ORANGE)
            s.text(9, 228, "OK move  </> song  UP/DN tempo 92", 1, GRAY)
            frames.append(s.out())
    save_gif(frames, "dance.gif", step_ms // 2)


# ---------------------------------------------------------------- screens.png
def usage_screen():
    s = Screen()
    logo(s, 20, 18, 11, 0, ORANGE)
    s.text(38, 11, "Claude Usage", 2, CREAM)
    s.rrect(200, 11, 26, 15, 4, ORANGE, outline=True)
    s.text(204, 15, "PRO", 1, ORANGE)
    for y, label, span, pct, reset in ((42, "Session", "(5hr)", 14, "3h 32m"), (102, "Weekly", "(7 day)", 2, "6d 20h")):
        s.text(12, y, label, 2, CREAM)
        s.text(12 + len(label) * 12 + 4, y + 6, span, 1, GRAY)
        p = f"{pct}%"
        s.text(228 - len(p) * 12, y, p, 2, ORANGE)
        s.rrect(12, y + 22, 216, 12, 6, TRACK)
        s.rrect(12, y + 22, max(12, 216 * pct // 100), 12, 6, ORANGE)
        s.text(12, y + 40, "Resets in " + reset, 1, GRAY)
    bot(s, 20, 180, 6, "happy", 2)
    bubble(s, 76, 164, 156, 46, 66, 192)
    s.text(85, 171, "Plenty of room. Let's", 1, DARK)
    s.text(85, 183, "build something!", 1, DARK)
    s.circle(10, 229, 3, GREEN)
    s.text(18, 226, "Updated 12s ago", 1, GRAY)
    s.text(128, 226, "<> page  OK reload", 1, GRAY)
    return s


def clock_screen():
    s = Screen()
    logo(s, 120, 24, 15, 0, ORANGE)
    s.text(21, 46, "7:42", 6, CREAM)
    s.text(169, 78, "AM", 2, ORANGE)
    s.centered("Mon, 05 Oct", 102, 2, GRAY)
    U, bx, by = 10, 85, 150
    bot(s, bx, by, U, "blink")
    s.tri([(bx + U, by), (bx + 5 * U, by), (bx + 6 * U, by - 2 * U)], BLUE)
    s.circle(bx + 6 * U, by - 2 * U, U // 2, CREAM)
    for i, p in enumerate((0.1, 0.45, 0.8)):  # same drift as drawSleepScreen()
        s.text(bx + 7 * U + 4 + int(p * 40), by - 16 - int(p * 24), "zzZ"[i], 1 + int(p * 3), CREAM)
    s.centered("Session 14%   Weekly 2%", 210, 1, GRAY)
    s.circle(92, 229, 4, ORANGE)
    s.rect(88, 229, 9, 3, ORANGE)
    s.text(102, 226, "8:00 AM", 1, ORANGE)
    return s


def alarm_screen():
    s = Screen()
    s.fill((58, 28, 24))
    logo(s, 120, 30, 24, 40, ORANGE)
    s.text(120 - 7 * 9, 60, "7:00 AM", 3, CREAM)
    bot(s, 78, 96, 12, "happy", 2, 2)
    for i, ch in enumerate("WAKE UP!"):
        s.text(48 + i * 18, 182 + int(5 * math.sin(i * 0.8)), ch, 3, ORANGE)
    s.text(12, 222, "OK: stop   any key/shake: snooze 5m", 1, CREAM)
    return s


def pet_screen():
    s = Screen()
    s.rrect(170, 30, 58, 44, 6, (58, 110, 165))
    s.circle(212, 44, 7, AMBER)
    s.rrect(170, 30, 58, 44, 6, CREAM, outline=True)
    s.rect(199, 30, 1, 44, CREAM)
    s.rect(170, 52, 58, 1, CREAM)
    s.rect(0, 200, 240, 40, (33, 33, 33))
    for x, v, icon in ((4, 85, 0), (82, 60, 1), (160, 35, 2)):
        if icon == 0:
            s.circle(x + 6, 12, 6, (201, 146, 90))
        elif icon == 1:
            s.tri([(x + 7, 6), (x + 2, 13), (x + 7, 13)], AMBER)
            s.tri([(x + 5, 11), (x + 10, 11), (x + 4, 19)], AMBER)
        else:
            logo(s, x + 6, 12, 7, 0, CREAM)
        s.rrect(x + 16, 8, 54, 8, 4, TRACK)
        s.rrect(x + 16, 8, max(8, 54 * v // 100), 8, 4, GREEN if v > 60 else AMBER if v > 30 else RED)
    bot(s, 71, 130, 14, "happy", 1, 1)
    s.circle(120, 110, 10, (201, 146, 90))
    bubble(s, 8, 30, 150, 44, 70, 118)
    s.text(18, 46, "Nom nom nom!", 1, DARK)
    s.centered("OK feed  > play  ^ tickle  v nap", 226, 1, GRAY)
    return s


def pomodoro_screen():
    s = Screen()
    done = 0.38
    for a in range(0, 360, 4):
        r = math.radians(a)
        s.circle(120 + math.sin(r) * 80, 104 - math.cos(r) * 80, 4, ORANGE if a < done * 360 else TRACK)
    s.centered("FOCUS", 50, 2, ORANGE)
    s.centered("15:30", 72, 4, CREAM)
    bot(s, 103, 120, 5, "normal", 1, 0)
    for i in range(3):
        x = 120 - 27 + 9 + i * 18
        s.circle(x, 200, 6, RED)
        s.rect(x - 1, 192, 3, 3, GREEN)
    s.centered("OK pause   < reset   UP/DN 25 min", 226, 1, GRAY)
    return s


def menu_screen():
    s = Screen()
    logo(s, 20, 18, 11, 0, ORANGE)
    s.text(38, 11, "Menu", 2, CREAM)
    items = ["Claude Usage", "Claude Ask", "Claude Dance", "Clawd Pet", "Alarms", "Face Finder"]
    for i, name in enumerate(items):
        y = 40 + i * 30
        if i == 2:
            s.rrect(10, y, 220, 26, 8, ORANGE)
        else:
            s.rrect(10, y, 220, 26, 8, TRACK, outline=True)
        s.text(22, y + 6, name, 2, DARK if i == 2 else CREAM)
    s.tri([(220, 222), (214, 214), (226, 214)], GRAY)
    s.text(48, 226, "UP/DOWN move   OK open", 1, GRAY)
    return s


def screens_png():
    shots = [("Menu", menu_screen()), ("Claude usage", usage_screen()), ("Clawd Pet", pet_screen()),
             ("Pomodoro", pomodoro_screen()), ("Idle clock", clock_screen()), ("Alarm!", alarm_screen())]
    pad, cap, size = 24, 34, 240 * SCALE
    sheet = Image.new("RGB", (3 * size + 4 * pad, 2 * (size + cap) + 3 * pad), (13, 17, 23))
    d = ImageDraw.Draw(sheet)
    for i, (title, scr) in enumerate(shots):
        x, y = pad + (i % 3) * (size + pad), pad + (i // 3) * (size + cap + pad)
        frame = Image.new("RGB", (size + 16, size + 16), (60, 60, 64))  # little bezel
        sheet.paste(frame, (x - 8, y - 8))
        sheet.paste(scr.out(), (x, y))
        label = Screen()  # draw the caption in the same pixel font
        cimg = Image.new("RGB", (size, 16), (13, 17, 23))
        cd = ImageDraw.Draw(cimg)
        label.img, label.d = cimg, cd
        label.text(size // 2 - len(title) * 6, 0, title, 2, (201, 209, 217))
        sheet.paste(cimg, (x, y + size + 14))
    sheet.save(OUT / "screens.png", optimize=True)
    print(f"screens.png: {(OUT / 'screens.png').stat().st_size // 1024} KB")


if __name__ == "__main__":
    boot_gif()
    dance_gif()
    screens_png()

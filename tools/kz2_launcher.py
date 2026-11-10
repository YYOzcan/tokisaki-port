#!/usr/bin/env python3
"""Tokisaki Port -- the Killzone 2 launcher (PyQt5).

Play, the settings, the controls, the saves and what the last session ran at,
the way an installed PC game opens. The settings live in
~/.config/tokisaki-port/kz2.json and become the environment of tools/play.sh
(the runtime's own switches, see game_env below). The game's artwork
(PIC1.PNG, ICON0.PNG) is read from the player's own game_dump/ at run time;
nothing of the game is in this file. Every icon is drawn here, so none goes
missing under an icon theme that lacks it.

    python3 tools/kz2_launcher.py        (tools/install_desktop_entry.sh adds it to the menu)
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

from PyQt5 import QtCore, QtGui, QtWidgets

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "game_dump" / "PS3_GAME"
CONFIG = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "tokisaki-port" / "kz2.json"
LOG = ROOT / "logs" / "play.log"
SAVES = ROOT / "run" / "gamedata"
BACKUPS = Path.home() / "Belgeler" / "KZ2-Yedek" / "kayitlar"

RED, RED_HOT, INK, PANEL, LINE, PALE, STEEL = "#B3121B", "#E0262F", "#0B0D10", "#13161B", "#262B33", "#E8E6E1", "#8A9099"

DEFAULTS = {
    "fullscreen": False,
    "vsync": "mailbox",          # fifo | mailbox | immediate
    "framegen": "0",             # 0 | 1 (PS3RECOMP_VK_FRAMEGEN)
    "post_blur": False,
    "mouse_sens": 3.0,
    "mouse_invert": False,
    "gyro_sens": 5.0,
    "gpu": "auto",               # auto | discrete | integrated
    "overlay": False,            # MangoHud
}


def load_settings():
    s = dict(DEFAULTS)
    try:
        s.update({k: v for k, v in json.loads(CONFIG.read_text()).items() if k in DEFAULTS})
    except (OSError, ValueError):
        pass
    return s


def save_settings(s):
    CONFIG.parent.mkdir(parents=True, exist_ok=True)
    CONFIG.write_text(json.dumps(s, indent=2))


def game_env(s):
    """The runtime switches for these settings (tools/play.sh adds the window)."""
    env = dict(os.environ)
    for k in ("PAD_SCRIPT", "KZ2_RUN_DIR", "KZ2_BUILD_DIR", "MANGOHUD"):
        env.pop(k, None)
    env["PS3RECOMP_VK_WINDOW"] = "1"
    env["PS3RECOMP_VK_PRESENT"] = s["vsync"]
    env["PS3RECOMP_VK_FRAMEGEN"] = s["framegen"]
    env["KZ2_FPS"] = "1"
    for key, var in (("fullscreen", "PS3RECOMP_VK_FULLSCREEN"), ("post_blur", "KZ2_POST_BLUR")):
        if s[key]:
            env[var] = "1"
        else:
            env.pop(var, None)
    env["PS3_PAD_MOUSE_SENS"] = f"{s['mouse_sens']:g}"
    env["PS3_PAD_MOUSE_INVERT"] = "1" if s["mouse_invert"] else "0"
    env["PS3_PAD_GYRO_SENS"] = f"{s['gyro_sens']:g}"
    if s["gpu"] == "auto":
        env.pop("KZ2_GPU", None)
    else:
        env["KZ2_GPU"] = s["gpu"]
    if s["overlay"] and shutil.which("mangohud"):
        env["MANGOHUD"] = "1"
    return env


def version():
    try:
        return subprocess.run(["git", "-C", str(ROOT), "describe", "--tags", "--always"],
                              capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


FPS_RE = re.compile(r"\[fps\] ([\d.]+) presented/s")


def log_fps(tail_bytes=None):
    """The [fps] values of play.log (KZ2_FPS=1 prints one every 5 s)."""
    try:
        with open(LOG, "rb") as f:
            if tail_bytes:
                f.seek(0, os.SEEK_END)
                f.seek(max(0, f.tell() - tail_bytes))
            text = f.read().decode(errors="replace")
    except OSError:
        return []
    return [float(m.group(1)) for m in FPS_RE.finditer(text)]


# ---- icons, drawn ------------------------------------------------------------

def icon_path(name, r):
    """A QPainterPath for one of the sidebar's icons in rect r."""
    p = QtGui.QPainterPath()
    x, y, w, h = r.x(), r.y(), r.width(), r.height()
    if name == "play":
        p.moveTo(x + w * 0.28, y + h * 0.18); p.lineTo(x + w * 0.82, y + h * 0.5)
        p.lineTo(x + w * 0.28, y + h * 0.82); p.closeSubpath()
    elif name == "stop":
        p.addRect(QtCore.QRectF(x + w * 0.25, y + h * 0.25, w * 0.5, h * 0.5))
    elif name == "display":
        p.addRoundedRect(QtCore.QRectF(x + w * 0.1, y + h * 0.18, w * 0.8, h * 0.5), 2, 2)
        p.addRect(QtCore.QRectF(x + w * 0.44, y + h * 0.68, w * 0.12, h * 0.1))
        p.addRect(QtCore.QRectF(x + w * 0.3, y + h * 0.78, w * 0.4, h * 0.06))
    elif name == "controls":
        p.addRoundedRect(QtCore.QRectF(x + w * 0.08, y + h * 0.3, w * 0.84, h * 0.44), h * 0.2, h * 0.2)
    elif name == "performance":
        p.moveTo(x + w * 0.1, y + h * 0.75); p.lineTo(x + w * 0.35, y + h * 0.45)
        p.lineTo(x + w * 0.55, y + h * 0.6); p.lineTo(x + w * 0.9, y + h * 0.22)
    elif name == "saves":
        p.addRoundedRect(QtCore.QRectF(x + w * 0.18, y + h * 0.14, w * 0.64, h * 0.72), 2, 2)
        p.addRect(QtCore.QRectF(x + w * 0.32, y + h * 0.14, w * 0.36, h * 0.24))
    elif name == "about":
        p.addEllipse(QtCore.QRectF(x + w * 0.14, y + h * 0.14, w * 0.72, h * 0.72))
    return p


def paint_icon(painter, name, r, color):
    painter.save()
    painter.setRenderHint(QtGui.QPainter.Antialiasing)
    pen = QtGui.QPen(QtGui.QColor(color), max(1.6, r.width() / 14.0))
    pen.setJoinStyle(QtCore.Qt.RoundJoin); pen.setCapStyle(QtCore.Qt.RoundCap)
    painter.setPen(pen)
    path = icon_path(name, r)
    if name in ("play", "stop"):
        painter.fillPath(path, QtGui.QColor(color))
    else:
        painter.drawPath(path)
    x, y, w, h = r.x(), r.y(), r.width(), r.height()
    if name == "controls":                       # d-pad and buttons on the pad
        painter.drawLine(QtCore.QPointF(x + w * 0.25, y + h * 0.52), QtCore.QPointF(x + w * 0.41, y + h * 0.52))
        painter.drawLine(QtCore.QPointF(x + w * 0.33, y + h * 0.44), QtCore.QPointF(x + w * 0.33, y + h * 0.6))
        painter.setBrush(QtGui.QColor(color))
        for cx, cy in ((0.66, 0.46), (0.76, 0.56)):
            painter.drawEllipse(QtCore.QPointF(x + w * cx, y + h * cy), w * 0.035, w * 0.035)
    elif name == "saves":
        painter.drawLine(QtCore.QPointF(x + w * 0.32, y + h * 0.62), QtCore.QPointF(x + w * 0.68, y + h * 0.62))
    elif name == "about":
        painter.drawLine(QtCore.QPointF(x + w * 0.5, y + h * 0.46), QtCore.QPointF(x + w * 0.5, y + h * 0.68))
        painter.setBrush(QtGui.QColor(color))
        painter.drawEllipse(QtCore.QPointF(x + w * 0.5, y + h * 0.33), w * 0.03, w * 0.03)
    painter.restore()


class NavButton(QtWidgets.QAbstractButton):
    def __init__(self, icon, text, parent=None):
        super().__init__(parent)
        self.icon_name, self.label = icon, text
        self.setCheckable(True); self.setCursor(QtCore.Qt.PointingHandCursor)
        self.setFixedHeight(54); self.setMinimumWidth(196)

    def paintEvent(self, _):
        p = QtGui.QPainter(self)
        p.setRenderHint(QtGui.QPainter.Antialiasing)
        hot = self.isChecked() or self.underMouse()
        if self.isChecked():
            p.fillRect(self.rect(), QtGui.QColor(255, 255, 255, 14))
            p.fillRect(0, 0, 3, self.height(), QtGui.QColor(RED_HOT))
        elif self.underMouse():
            p.fillRect(self.rect(), QtGui.QColor(255, 255, 255, 7))
        color = PALE if hot else STEEL
        paint_icon(p, self.icon_name, QtCore.QRectF(22, (self.height() - 22) / 2, 22, 22), color)
        f = self.font(); f.setPointSizeF(10.5); f.setBold(self.isChecked()); f.setLetterSpacing(QtGui.QFont.AbsoluteSpacing, 1.5)
        p.setFont(f); p.setPen(QtGui.QColor(color))
        p.drawText(QtCore.QRectF(60, 0, self.width() - 60, self.height()), QtCore.Qt.AlignVCenter, self.label)

    def enterEvent(self, e):
        self.update(); super().enterEvent(e)

    def leaveEvent(self, e):
        self.update(); super().leaveEvent(e)


class PlayButton(QtWidgets.QPushButton):
    """OYNA / DURDUR, with a drawn triangle or square."""
    def __init__(self):
        super().__init__()
        self.running = False
        self.setCursor(QtCore.Qt.PointingHandCursor); self.setFixedSize(300, 76)

    def set_running(self, on):
        self.running = on; self.update()

    def paintEvent(self, _):
        p = QtGui.QPainter(self)
        p.setRenderHint(QtGui.QPainter.Antialiasing)
        base = QtGui.QColor("#2a2f36" if self.running else RED)
        if self.underMouse():
            base = QtGui.QColor("#3a4049" if self.running else RED_HOT)
        if not self.isEnabled():
            base = QtGui.QColor("#3a1a1c")
        p.setBrush(base); p.setPen(QtCore.Qt.NoPen); p.drawRoundedRect(QtCore.QRectF(self.rect()), 4, 4)
        paint_icon(p, "stop" if self.running else "play", QtCore.QRectF(38, 22, 32, 32), PALE)
        f = self.font(); f.setPointSize(19); f.setBold(True); f.setLetterSpacing(QtGui.QFont.AbsoluteSpacing, 6)
        p.setFont(f); p.setPen(QtGui.QColor(PALE))
        p.drawText(QtCore.QRectF(84, 0, self.width() - 100, self.height()), QtCore.Qt.AlignVCenter,
                   "DURDUR" if self.running else "OYNA")

    def enterEvent(self, e):
        self.update(); super().enterEvent(e)

    def leaveEvent(self, e):
        self.update(); super().leaveEvent(e)


QSS = f"""
QWidget {{ color: {PALE}; font-size: 10.5pt; }}
QWidget#side {{ background: #090a0d; border-right: 1px solid {LINE}; }}
QScrollArea, QWidget#page {{ background: transparent; border: none; }}
QLabel#h1 {{ font-size: 20pt; font-weight: 800; letter-spacing: 4px; }}
QLabel#h2 {{ color: {STEEL}; font-size: 9pt; font-weight: 700; letter-spacing: 3px; margin-top: 14px; }}
QLabel#note {{ color: {STEEL}; font-size: 9.5pt; }}
QFrame#card {{ background: {PANEL}; border: 1px solid {LINE}; border-radius: 4px; }}
QCheckBox {{ spacing: 10px; }}
QCheckBox::indicator {{ width: 18px; height: 18px; border: 1px solid #4a515a; background: #15181d; border-radius: 2px; }}
QCheckBox::indicator:checked {{ background: {RED_HOT}; border-color: {RED_HOT}; }}
QComboBox {{ background: #15181d; border: 1px solid {LINE}; padding: 6px 10px; min-width: 260px; }}
QComboBox QAbstractItemView {{ background: #15181d; selection-background-color: {RED}; }}
QSlider::groove:horizontal {{ height: 4px; background: {LINE}; }}
QSlider::sub-page:horizontal {{ background: {RED}; }}
QSlider::handle:horizontal {{ background: {PALE}; width: 14px; margin: -6px 0; border-radius: 7px; }}
QPushButton {{ background: #1b1f25; border: 1px solid {LINE}; padding: 8px 18px; letter-spacing: 1px; }}
QPushButton:hover {{ border-color: {RED_HOT}; }}
QScrollBar:vertical {{ background: transparent; width: 10px; margin: 0; }}
QScrollBar::handle:vertical {{ background: {LINE}; min-height: 30px; border-radius: 4px; }}
QScrollBar::handle:vertical:hover {{ background: #3a414b; }}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {{ background: transparent; }}
QTableWidget {{ background: {PANEL}; border: 1px solid {LINE}; gridline-color: {LINE}; }}
QHeaderView::section {{ background: #15181d; color: {STEEL}; border: none; padding: 6px; }}
"""


def card():
    f = QtWidgets.QFrame(objectName="card")
    lay = QtWidgets.QVBoxLayout(f); lay.setContentsMargins(20, 16, 20, 16); lay.setSpacing(12)
    return f, lay


def heading(text):
    return QtWidgets.QLabel(text, objectName="h2")


def note(text):
    l = QtWidgets.QLabel(text, objectName="note"); l.setWordWrap(True)
    return l


def row(label, widget):
    w = QtWidgets.QWidget(); h = QtWidgets.QHBoxLayout(w); h.setContentsMargins(0, 0, 0, 0)
    l = QtWidgets.QLabel(label); h.addWidget(l); h.addStretch(1); h.addWidget(widget)
    return w


class SliderRow(QtWidgets.QWidget):
    changed = QtCore.pyqtSignal(float)

    def __init__(self, label, lo, hi, step, value):
        super().__init__()
        self.step = step
        h = QtWidgets.QHBoxLayout(self); h.setContentsMargins(0, 0, 0, 0)
        h.addWidget(QtWidgets.QLabel(label)); h.addStretch(1)
        self.s = QtWidgets.QSlider(QtCore.Qt.Horizontal); self.s.setFixedWidth(260)
        self.s.setRange(int(round(lo / step)), int(round(hi / step))); self.s.setValue(int(round(value / step)))
        self.v = QtWidgets.QLabel(); self.v.setFixedWidth(44); self.v.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        h.addWidget(self.s); h.addWidget(self.v)
        self.s.valueChanged.connect(self._moved); self._moved(self.s.value())

    def _moved(self, n):
        self.v.setText(f"{n * self.step:g}"); self.changed.emit(n * self.step)

    def set_value(self, value):
        self.s.setValue(int(round(value / self.step)))


class Page(QtWidgets.QScrollArea):
    def __init__(self, title):
        super().__init__()
        self.setWidgetResizable(True)
        inner = QtWidgets.QWidget(objectName="page")
        self.lay = QtWidgets.QVBoxLayout(inner); self.lay.setContentsMargins(44, 36, 44, 36); self.lay.setSpacing(10)
        self.lay.addWidget(QtWidgets.QLabel(title, objectName="h1"))
        self.setWidget(inner)

    def finish(self):
        self.lay.addStretch(1)


CONTROLS = [
    ("W A S D", "Hareket"), ("Fare", "Bakış"), ("Sol tık", "Ateş (R1)"), ("Sağ tık", "Nişan (L1)"),
    ("Boşluk", "Zıpla"), ("C", "Eğil"), ("Z", "Siper (L2)"), ("Sol Shift", "Koş"),
    ("R / F", "Şarjör"), ("T", "Silah değiştir"), ("G", "Bomba (R2)"), ("V / orta tık", "Yakın dövüş"),
    ("Q / E", "L1 / R1"), ("Yön tuşları", "D-pad"), ("Ctrl + fare", "SIXAXIS"), ("Enter", "Duraklat (START)"),
    ("Tab", "SELECT"), ("Esc / Backspace", "Geri (Daire)"), ("F1", "Fareyi bırak / yakala"),
]


class Launcher(QtWidgets.QWidget):
    def __init__(self):
        super().__init__()
        self.s = load_settings()
        self.proc = None
        self.started = 0.0
        self.setWindowTitle("Killzone 2 — Tokisaki Port")
        if (GAME / "ICON0.PNG").exists():
            self.setWindowIcon(QtGui.QIcon(str(GAME / "ICON0.PNG")))
        self.bg = QtGui.QPixmap(str(GAME / "PIC1.PNG")) if (GAME / "PIC1.PNG").exists() else QtGui.QPixmap()
        self.setMinimumSize(1120, 660)

        side = QtWidgets.QWidget(objectName="side"); side.setFixedWidth(220)
        sl = QtWidgets.QVBoxLayout(side); sl.setContentsMargins(0, 28, 0, 20); sl.setSpacing(2)
        logo = QtWidgets.QLabel("KILLZONE <span style='color:%s'>2</span>" % RED_HOT)
        logo.setStyleSheet("font-size: 17pt; font-weight: 800; letter-spacing: 3px; padding-left: 22px;")
        sub = QtWidgets.QLabel("TOKISAKI PORT"); sub.setStyleSheet(f"color: {STEEL}; font-size: 8pt; letter-spacing: 5px; padding-left: 23px;")
        sl.addWidget(logo); sl.addWidget(sub); sl.addSpacing(26)
        self.stack = QtWidgets.QStackedWidget()
        self.nav = QtWidgets.QButtonGroup(self); self.nav.setExclusive(True)
        pages = [("play", "OYNA", self.home_page()), ("display", "GÖRÜNTÜ", self.display_page()),
                 ("controls", "KONTROLLER", self.controls_page()), ("performance", "PERFORMANS", self.perf_page()),
                 ("saves", "KAYITLAR", self.saves_page()), ("about", "HAKKINDA", self.about_page())]
        for i, (ic, text, page) in enumerate(pages):
            b = NavButton(ic, text); self.nav.addButton(b, i); sl.addWidget(b); self.stack.addWidget(page)
        self.nav.buttonClicked[int].connect(self.stack.setCurrentIndex)
        self.stack.currentChanged.connect(lambda _: self.update())
        self.nav.button(0).setChecked(True)
        sl.addStretch(1)
        self.side_status = QtWidgets.QLabel(); self.side_status.setStyleSheet(f"color: {STEEL}; font-size: 8.5pt; padding-left: 23px;")
        sl.addWidget(self.side_status)

        lay = QtWidgets.QHBoxLayout(self); lay.setContentsMargins(0, 0, 0, 0); lay.setSpacing(0)
        lay.addWidget(side); lay.addWidget(self.stack, 1)

        QtWidgets.QShortcut(QtGui.QKeySequence(QtCore.Qt.Key_Return), self, activated=self.toggle_game)
        self.timer = QtCore.QTimer(self); self.timer.timeout.connect(self.poll); self.timer.start(1000)
        self.set_status("HAZIR")
        self.refresh_last_session()

    # ---- pages ---------------------------------------------------------------
    def home_page(self):
        w = QtWidgets.QWidget(objectName="page")
        v = QtWidgets.QVBoxLayout(w); v.setContentsMargins(56, 40, 56, 32)
        v.addStretch(3)
        t = QtWidgets.QLabel("KILLZONE <span style='color:%s'>2</span>" % RED_HOT)
        t.setStyleSheet("font-size: 52pt; font-weight: 800; letter-spacing: 6px;")
        st = QtWidgets.QLabel("TOKISAKI PORT  ·  PC"); st.setStyleSheet(f"color: {STEEL}; font-size: 11pt; letter-spacing: 8px;")
        v.addWidget(t); v.addWidget(st); v.addSpacing(30)
        self.play = PlayButton(); self.play.clicked.connect(self.toggle_game)
        r = QtWidgets.QHBoxLayout(); r.addWidget(self.play); r.addSpacing(24)
        info = QtWidgets.QVBoxLayout(); info.setSpacing(2)
        self.status = QtWidgets.QLabel(); self.live = QtWidgets.QLabel(); self.live.setStyleSheet(f"color: {STEEL};")
        info.addStretch(1); info.addWidget(self.status); info.addWidget(self.live); info.addStretch(1)
        r.addLayout(info); r.addStretch(1)
        v.addLayout(r); v.addSpacing(10)
        self.last = QtWidgets.QLabel(); self.last.setStyleSheet(f"color: {STEEL}; font-size: 9.5pt;")
        v.addWidget(self.last)
        v.addStretch(4)
        foot = QtWidgets.QLabel("Enter: oyna / durdur   ·   ps3recomp ile statik yeniden derleme   ·   "
                                "Killzone 2 © Sony Interactive Entertainment")
        foot.setStyleSheet(f"color: {STEEL}; font-size: 8pt;")
        v.addWidget(foot)
        self.home = w
        return w

    def display_page(self):
        p = Page("GÖRÜNTÜ")
        c, l = card()
        self.full = QtWidgets.QCheckBox("Tam ekran (pencereli tam ekran)")
        self.full.setChecked(self.s["fullscreen"]); self.full.toggled.connect(lambda on: self.set("fullscreen", on))
        l.addWidget(self.full)
        self.vsync = QtWidgets.QComboBox()
        for label, key in (("Düşük gecikme (önerilen)", "mailbox"), ("Açık — ekranla eşit, yırtılma yok", "fifo"),
                           ("Kapalı — yırtılma olabilir", "immediate")):
            self.vsync.addItem(label, key)
        self.vsync.setCurrentIndex(max(0, self.vsync.findData(self.s["vsync"])))
        self.vsync.currentIndexChanged.connect(lambda _: self.set("vsync", self.vsync.currentData()))
        l.addWidget(row("Dikey senkron", self.vsync))
        p.lay.addWidget(c)
        p.lay.addWidget(heading("AKICILIK"))
        c, l = card()
        self.fg = QtWidgets.QComboBox()
        for label, key in (("Kapalı", "0"), ("Açık — iki kare arasına bir kare", "1")):
            self.fg.addItem(label, key)
        self.fg.setCurrentIndex(max(0, self.fg.findData(self.s["framegen"])))
        self.fg.currentIndexChanged.connect(lambda _: self.set("framegen", self.fg.currentData()))
        l.addWidget(row("Kare üretimi", self.fg))
        l.addWidget(note("Ekranda gösterilen kare sayısını ikiye katlar. Ara kare, iki gerçek kare arasındaki "
                         "hareketten üretilir; yüksek yenileme hızlı ekranlarda en iyi sonucu verir."))
        p.lay.addWidget(c)
        p.lay.addWidget(heading("EFEKTLER"))
        c, l = card()
        self.blur = QtWidgets.QCheckBox("Hareket bulanıklığı ve alan derinliği (PS3'teki gibi, daha yavaş)")
        self.blur.setChecked(self.s["post_blur"]); self.blur.toggled.connect(lambda on: self.set("post_blur", on))
        l.addWidget(self.blur)
        p.lay.addWidget(c)
        p.finish()
        return p

    def controls_page(self):
        p = Page("KONTROLLER")
        c, l = card()
        self.sens = SliderRow("Fare hassasiyeti", 0.5, 12.0, 0.5, self.s["mouse_sens"])
        self.sens.changed.connect(lambda v: self.set("mouse_sens", v)); l.addWidget(self.sens)
        self.inv = QtWidgets.QCheckBox("Fare Y eksenini ters çevir"); self.inv.setChecked(self.s["mouse_invert"])
        self.inv.toggled.connect(lambda on: self.set("mouse_invert", on)); l.addWidget(self.inv)
        self.gyro = SliderRow("SIXAXIS (Ctrl + fare) hassasiyeti", 1.0, 20.0, 0.5, self.s["gyro_sens"])
        self.gyro.changed.connect(lambda v: self.set("gyro_sens", v)); l.addWidget(self.gyro)
        l.addWidget(note("Kontrolcü (DualShock, DualSense, Xbox) takılıysa doğrudan çalışır."))
        p.lay.addWidget(c)
        p.lay.addWidget(heading("KLAVYE VE FARE"))
        t = QtWidgets.QTableWidget(len(CONTROLS), 2)
        t.setHorizontalHeaderLabels(["Tuş", "Eylem"]); t.verticalHeader().setVisible(False)
        t.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers); t.setSelectionMode(QtWidgets.QAbstractItemView.NoSelection)
        t.setFocusPolicy(QtCore.Qt.NoFocus)
        for i, (k, a) in enumerate(CONTROLS):
            ki = QtWidgets.QTableWidgetItem(k); f = ki.font(); f.setBold(True); ki.setFont(f)
            t.setItem(i, 0, ki); t.setItem(i, 1, QtWidgets.QTableWidgetItem(a))
        t.horizontalHeader().setSectionResizeMode(0, QtWidgets.QHeaderView.ResizeToContents)
        t.horizontalHeader().setStretchLastSection(True)
        t.setFixedHeight(t.verticalHeader().length() + t.horizontalHeader().height() + 4)
        p.lay.addWidget(t)
        p.finish()
        return p

    def perf_page(self):
        p = Page("PERFORMANS")
        c, l = card()
        self.gpu = QtWidgets.QComboBox()
        for label, key in (("Otomatik", "auto"), ("Harici ekran kartı", "discrete"), ("Dahili ekran kartı", "integrated")):
            self.gpu.addItem(label, key)
        self.gpu.setCurrentIndex(max(0, self.gpu.findData(self.s["gpu"])))
        self.gpu.currentIndexChanged.connect(lambda _: self.set("gpu", self.gpu.currentData()))
        l.addWidget(row("Ekran kartı", self.gpu))
        self.overlay = QtWidgets.QCheckBox("Oyunda FPS göstergesi (MangoHud)")
        self.overlay.setChecked(self.s["overlay"]); self.overlay.toggled.connect(lambda on: self.set("overlay", on))
        if not shutil.which("mangohud"):
            self.overlay.setEnabled(False); self.overlay.setText("Oyunda FPS göstergesi (MangoHud kurulu değil)")
        l.addWidget(self.overlay)
        p.lay.addWidget(c)
        p.lay.addWidget(heading("SON OTURUM"))
        c, l = card()
        self.perf_info = QtWidgets.QLabel(); self.perf_info.setWordWrap(True); l.addWidget(self.perf_info)
        self.graph = FpsGraph(); l.addWidget(self.graph)
        p.lay.addWidget(c)
        p.finish()
        return p

    def saves_page(self):
        p = Page("KAYITLAR")
        c, l = card()
        self.save_list = QtWidgets.QLabel(); self.save_list.setWordWrap(True); l.addWidget(self.save_list)
        b = QtWidgets.QHBoxLayout()
        for text, fn in (("Şimdi yedekle", self.backup_saves), ("Yedekten geri yükle", self.restore_saves),
                         ("Kayıt klasörünü aç", lambda: self.open_dir(SAVES)), ("Yedek klasörünü aç", lambda: self.open_dir(BACKUPS))):
            btn = QtWidgets.QPushButton(text); btn.clicked.connect(fn); b.addWidget(btn)
        b.addStretch(1); l.addLayout(b)
        l.addWidget(note(f"Yedekler {BACKUPS} altına tarih adıyla yazılır. Geri yüklemeden önce mevcut kayıtlar "
                         "otomatik olarak yedeklenir."))
        p.lay.addWidget(c)
        p.finish()
        self.refresh_saves()
        return p

    def about_page(self):
        p = Page("HAKKINDA")
        c, l = card()
        exe = ROOT / "build" / "killzone2"
        built = time.strftime("%d.%m.%Y %H:%M", time.localtime(exe.stat().st_mtime)) if exe.exists() else "derlenmemiş"
        l.addWidget(QtWidgets.QLabel(f"<b>Sürüm</b>  {version() or '?'}"))
        l.addWidget(QtWidgets.QLabel(f"<b>Oyun derlemesi</b>  {built}"))
        l.addWidget(note("Killzone 2'nin PS3 kodu ps3recomp ile önceden x86-64'e çevrilir; SPU işleri ve RSX "
                         "grafikleri bilgisayarda çalıştırılır (Vulkan). Oyun dosyaları senin kendi kopyandan okunur."))
        b = QtWidgets.QHBoxLayout()
        for text, fn in (("Günlük klasörünü aç", lambda: self.open_dir(LOG.parent)),
                         ("Ayar dosyasını aç", lambda: self.open_dir(CONFIG.parent)),
                         ("Varsayılan ayarlara dön", self.reset_settings)):
            btn = QtWidgets.QPushButton(text); btn.clicked.connect(fn); b.addWidget(btn)
        b.addStretch(1); l.addLayout(b)
        p.lay.addWidget(c)
        p.finish()
        return p

    # ---- behaviour -----------------------------------------------------------
    def set(self, key, value):
        self.s[key] = value; save_settings(self.s)

    def reset_settings(self):
        self.s = dict(DEFAULTS); save_settings(self.s)
        self.full.setChecked(self.s["fullscreen"]); self.vsync.setCurrentIndex(self.vsync.findData(self.s["vsync"]))
        self.fg.setCurrentIndex(self.fg.findData(self.s["framegen"])); self.blur.setChecked(self.s["post_blur"])
        self.sens.set_value(self.s["mouse_sens"]); self.inv.setChecked(self.s["mouse_invert"])
        self.gyro.set_value(self.s["gyro_sens"]); self.gpu.setCurrentIndex(self.gpu.findData(self.s["gpu"]))
        self.overlay.setChecked(self.s["overlay"])
        self.set_status("AYARLAR VARSAYILANA DÖNDÜ")

    def open_dir(self, d):
        Path(d).mkdir(parents=True, exist_ok=True)
        QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(str(d)))

    def set_status(self, text, color=STEEL):
        self.status.setText(f"<span style='color:{color}'>●</span>  {text}")
        self.side_status.setText(text)

    def paintEvent(self, _):
        p = QtGui.QPainter(self)
        p.fillRect(self.rect(), QtGui.QColor(INK))
        if not self.bg.isNull() and self.stack.currentIndex() == 0:
            r = self.stack.geometry()
            pm = self.bg.scaled(r.size(), QtCore.Qt.KeepAspectRatioByExpanding, QtCore.Qt.SmoothTransformation)
            p.drawPixmap(r.x() + (r.width() - pm.width()) // 2, r.y() + (r.height() - pm.height()) // 2, pm)
            g = QtGui.QLinearGradient(r.x(), 0, r.right(), 0)
            g.setColorAt(0.0, QtGui.QColor(11, 13, 16, 245)); g.setColorAt(0.5, QtGui.QColor(11, 13, 16, 190))
            g.setColorAt(1.0, QtGui.QColor(11, 13, 16, 40))
            p.fillRect(r, g)
        p.fillRect(0, self.height() - 3, self.width(), 3, QtGui.QColor(RED))

    def toggle_game(self):
        if self.proc and self.proc.poll() is None:
            self.stop_game()
        else:
            self.launch()

    def launch(self):
        if not (ROOT / "build" / "killzone2").exists():
            QtWidgets.QMessageBox.warning(self, "Killzone 2", "Oyun henüz derlenmemiş (build/killzone2 yok).\n"
                                          "KZ2_MAX=1 tools/build.sh ile derleyin.")
            return
        LOG.parent.mkdir(parents=True, exist_ok=True)
        log = open(LOG, "w")
        self.proc = subprocess.Popen([str(ROOT / "tools" / "play.sh")], env=game_env(self.s), cwd=str(ROOT),
                                     stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        self.started = time.time()
        self.play.set_running(True); self.set_status("OYUN ÇALIŞIYOR", RED_HOT)
        self.update()

    def stop_game(self):
        if not self.proc:
            return
        try:
            os.killpg(self.proc.pid, 15)
        except OSError:
            pass
        self.set_status("KAPATILIYOR…")

    def poll(self):
        if self.proc and self.proc.poll() is None:
            ran = int(time.time() - self.started)
            fps = log_fps(65536)
            self.live.setText(f"Süre {ran // 60}:{ran % 60:02d}" + (f"   ·   {fps[-1]:.0f} FPS" if fps else ""))
            return
        if not self.proc:
            return
        rc, ran = self.proc.returncode, time.time() - self.started
        self.proc = None
        self.play.set_running(False); self.live.setText("")
        if self.isMinimized():
            self.showNormal()
        self.activateWindow()
        if rc not in (0, -15, 143) and ran < 20:
            tail = ""
            try:
                tail = "\n".join(LOG.read_text(errors="replace").splitlines()[-12:])
            except OSError:
                pass
            self.set_status("OYUN BAŞLATILAMADI", RED_HOT)
            QtWidgets.QMessageBox.warning(self, "Killzone 2", f"Oyun kapandı (kod {rc}).\n\n{tail}")
        else:
            self.set_status("HAZIR")
        self.refresh_last_session()

    def refresh_last_session(self):
        fps = log_fps()
        # gameplay only: past the boot, and below the 200-500 frames/s the menus run at
        play = [f for f in fps[2:] if 5.0 < f < 120.0] if len(fps) > 2 else []
        if not play:
            self.last.setText(""); self.perf_info.setText("Henüz bir oturum yok.") if hasattr(self, "perf_info") else None
            return
        avg = sum(play) / len(play)
        lo = sorted(play)[max(0, len(play) // 20)]
        self.last.setText(f"Son oturum: ortalama {avg:.0f} FPS")
        if hasattr(self, "perf_info"):
            self.perf_info.setText(f"Ortalama <b>{avg:.1f}</b> FPS   ·   en düşük %5: <b>{lo:.1f}</b>   ·   "
                                   f"{len(play) * 5 // 60} dk ölçüm (5 saniyelik pencereler)")
            self.graph.set_values(play)

    def refresh_saves(self):
        items = []
        if SAVES.exists():
            for d in sorted(SAVES.rglob("*")):
                if d.is_dir() and d.parent.name.lower() == "savedata":
                    mt = max((f.stat().st_mtime for f in d.rglob("*") if f.is_file()), default=d.stat().st_mtime)
                    items.append((mt, d.name))
        if not items:
            self.save_list.setText("Kayıt bulunamadı.")
        else:
            items.sort(reverse=True)
            self.save_list.setText("<br>".join(f"<b>{n}</b>  <span style='color:{STEEL}'>"
                                               f"{time.strftime('%d.%m.%Y %H:%M', time.localtime(t))}</span>" for t, n in items[:8]))

    def backup_saves(self, quiet=False):
        if not SAVES.exists():
            QtWidgets.QMessageBox.information(self, "Kayıtlar", "Yedeklenecek kayıt yok."); return None
        BACKUPS.mkdir(parents=True, exist_ok=True)
        out = BACKUPS / time.strftime("kz2-kayit-%Y%m%d-%H%M%S.tar.gz")
        with tarfile.open(out, "w:gz") as tf:
            tf.add(SAVES, arcname="gamedata")
        if not quiet:
            self.set_status("KAYITLAR YEDEKLENDİ")
            QtWidgets.QMessageBox.information(self, "Kayıtlar", f"Yedek alındı:\n{out}")
        return out

    def restore_saves(self):
        if self.proc and self.proc.poll() is None:
            QtWidgets.QMessageBox.warning(self, "Kayıtlar", "Önce oyunu kapatın."); return
        f, _ = QtWidgets.QFileDialog.getOpenFileName(self, "Yedek seç", str(BACKUPS), "Kayıt yedeği (*.tar.gz)")
        if not f:
            return
        if QtWidgets.QMessageBox.question(self, "Kayıtlar", "Mevcut kayıtlar bu yedekle değiştirilsin mi?\n"
                                          "(Önce mevcut kayıtların yedeği alınır.)") != QtWidgets.QMessageBox.Yes:
            return
        if SAVES.exists():
            self.backup_saves(quiet=True)
        tmp = SAVES.parent / "gamedata.restore"
        shutil.rmtree(tmp, ignore_errors=True); tmp.mkdir()
        with tarfile.open(f, "r:gz") as tf:
            tf.extractall(tmp, filter="data")
        src = tmp / "gamedata"
        if not src.is_dir():
            shutil.rmtree(tmp, ignore_errors=True)
            QtWidgets.QMessageBox.warning(self, "Kayıtlar", "Bu dosya bir kayıt yedeği değil."); return
        shutil.rmtree(SAVES, ignore_errors=True); src.rename(SAVES); shutil.rmtree(tmp, ignore_errors=True)
        self.refresh_saves(); self.set_status("KAYITLAR GERİ YÜKLENDİ")


class FpsGraph(QtWidgets.QWidget):
    """The last session's 5-second fps windows, a line over a 30 / 60 grid."""
    def __init__(self):
        super().__init__(); self.vals = []; self.setFixedHeight(120)

    def set_values(self, v):
        self.vals = list(v)[-120:]; self.update()

    def paintEvent(self, _):
        p = QtGui.QPainter(self); p.setRenderHint(QtGui.QPainter.Antialiasing)
        w, h = self.width(), self.height()
        top = max(60.0, max(self.vals, default=60.0) * 1.1)
        p.setPen(QtGui.QPen(QtGui.QColor(LINE), 1, QtCore.Qt.DashLine))
        f = self.font(); f.setPointSizeF(8); p.setFont(f)
        for mark in (30, 60):
            y = h - mark / top * h
            p.drawLine(QtCore.QPointF(30, y), QtCore.QPointF(w, y))
            p.setPen(QtGui.QColor(STEEL)); p.drawText(QtCore.QPointF(0, y + 4), str(mark))
            p.setPen(QtGui.QPen(QtGui.QColor(LINE), 1, QtCore.Qt.DashLine))
        if len(self.vals) < 2:
            return
        path = QtGui.QPainterPath()
        for i, v in enumerate(self.vals):
            pt = QtCore.QPointF(30 + i * (w - 30) / (len(self.vals) - 1), h - v / top * h)
            path.moveTo(pt) if i == 0 else path.lineTo(pt)
        p.setPen(QtGui.QPen(QtGui.QColor(RED_HOT), 2)); p.drawPath(path)


def main():
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName("Killzone 2"); app.setDesktopFileName("killzone2-tokisaki")
    app.setStyle("Fusion")                       # the same look under any desktop theme
    app.setStyleSheet(QSS)
    w = Launcher(); w.resize(1280, 740); w.show()
    if len(sys.argv) > 2 and sys.argv[1] == "--shot":   # screenshots of every page, for checking the layout
        def shots():
            for i in range(w.stack.count()):
                w.nav.button(i).click(); app.processEvents()
                w.grab().save(f"{sys.argv[2]}_{i}.png")
            app.quit()
        QtCore.QTimer.singleShot(300, shots)
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()

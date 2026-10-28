#!/usr/bin/env python3
"""Tokisaki Port -- the Killzone 2 launcher (PyQt5).

Play, settings and the controls at a glance, the way an installed PC game
opens. The settings live in ~/.config/tokisaki-port/kz2.json and become the
environment of tools/play.sh (they are the runtime's own switches, listed in
SETTINGS below). The game's artwork (PIC1.PNG, ICON0.PNG) is read from the
player's own game_dump/ at run time; nothing of the game is in this file.

    python3 tools/kz2_launcher.py        (tools/install_desktop_entry.sh adds it to the menu)
"""
import json
import os
import subprocess
import sys
import time
from pathlib import Path

from PyQt5 import QtCore, QtGui, QtWidgets

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "game_dump" / "PS3_GAME"
CONFIG = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "tokisaki-port" / "kz2.json"
LOG = ROOT / "logs" / "play.log"

RED, RED_HOT, INK, PALE, STEEL = "#B3121B", "#E0262F", "#0B0D10", "#E8E6E1", "#8A9099"

DEFAULTS = {
    "fullscreen": False,
    "vsync": "mailbox",          # fifo | mailbox | immediate
    "post_blur": False,
    "mouse_sens": 3.0,
    "mouse_invert": False,
    "gyro_sens": 5.0,
    "gpu": "auto",               # auto | discrete | integrated
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
    for k in ("PAD_SCRIPT", "KZ2_RUN_DIR", "KZ2_BUILD_DIR"):
        env.pop(k, None)
    env["PS3RECOMP_VK_WINDOW"] = "1"
    env["PS3RECOMP_VK_PRESENT"] = s["vsync"]
    if s["fullscreen"]:
        env["PS3RECOMP_VK_FULLSCREEN"] = "1"
    else:
        env.pop("PS3RECOMP_VK_FULLSCREEN", None)
    if s["post_blur"]:
        env["KZ2_POST_BLUR"] = "1"
    else:
        env.pop("KZ2_POST_BLUR", None)
    env["PS3_PAD_MOUSE_SENS"] = f"{s['mouse_sens']:g}"
    env["PS3_PAD_MOUSE_INVERT"] = "1" if s["mouse_invert"] else "0"
    env["PS3_PAD_GYRO_SENS"] = f"{s['gyro_sens']:g}"
    if s["gpu"] == "auto":
        env.pop("KZ2_GPU", None)
    else:
        env["KZ2_GPU"] = s["gpu"]
    return env


def version():
    try:
        return subprocess.run(["git", "-C", str(ROOT), "describe", "--tags", "--always"],
                              capture_output=True, text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


CONTROLS = [
    ("W A S D", "hareket"), ("Fare", "bakış"), ("Sol tık", "ateş"), ("Sağ tık", "nişan"),
    ("Boşluk", "zıpla"), ("C", "eğil"), ("Z", "siper"), ("Sol Shift", "koş"),
    ("R", "şarjör"), ("T", "silah değiştir"), ("G", "bomba"), ("V", "yakın dövüş"),
    ("Ctrl + fare", "SIXAXIS"), ("Enter", "duraklat"), ("F1", "fareyi bırak"),
]

QSS = f"""
QWidget {{ color: {PALE}; font-size: 11pt; }}
QDialog, QTabWidget::pane, QWidget#page {{ background: {INK}; }}
QTabBar::tab {{ background: #15181d; color: {STEEL}; padding: 8px 18px; border: none; }}
QTabBar::tab:selected {{ background: {INK}; color: {PALE}; border-bottom: 2px solid {RED_HOT}; }}
QCheckBox, QLabel, QRadioButton {{ background: transparent; }}
QCheckBox::indicator {{ width: 16px; height: 16px; border: 1px solid #4a515a; background: #15181d; }}
QCheckBox::indicator:checked {{ background: {RED_HOT}; border-color: {RED_HOT}; }}
QComboBox, QDoubleSpinBox {{ background: #15181d; border: 1px solid #2a2f36; padding: 4px 8px; }}
QSlider::groove:horizontal {{ height: 4px; background: #2a2f36; }}
QSlider::handle:horizontal {{ background: {RED_HOT}; width: 14px; margin: -6px 0; border-radius: 7px; }}
QPushButton {{ background: #1b1f25; border: 1px solid #2a2f36; padding: 8px 22px; letter-spacing: 1px; }}
QPushButton:hover {{ border-color: {RED_HOT}; }}
QPushButton#play {{ background: {RED}; border: none; font-size: 20pt; font-weight: 700;
                    letter-spacing: 6px; padding: 14px 64px; }}
QPushButton#play:hover {{ background: {RED_HOT}; }}
QPushButton#play:disabled {{ background: #3a1a1c; color: {STEEL}; }}
"""


class Settings(QtWidgets.QDialog):
    def __init__(self, s, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Ayarlar")
        self.s = dict(s)
        tabs = QtWidgets.QTabWidget()

        disp = QtWidgets.QWidget(objectName="page"); f = QtWidgets.QFormLayout(disp)
        self.full = QtWidgets.QCheckBox("Tam ekran (pencereli tam ekran)"); self.full.setChecked(s["fullscreen"])
        self.vsync = QtWidgets.QComboBox()
        for label, key in (("Açık  (ekranla eşit, yırtılma yok)", "fifo"),
                           ("Düşük gecikme  (önerilen)", "mailbox"),
                           ("Kapalı  (yırtılma olabilir)", "immediate")):
            self.vsync.addItem(label, key)
        self.vsync.setCurrentIndex(max(0, self.vsync.findData(s["vsync"])))
        self.blur = QtWidgets.QCheckBox("Hareket bulanıklığı ve alan derinliği (PS3'teki gibi, daha yavaş)")
        self.blur.setChecked(s["post_blur"])
        f.addRow(self.full); f.addRow("Dikey senkron", self.vsync); f.addRow(self.blur)
        tabs.addTab(disp, "GÖRÜNTÜ")

        ctl = QtWidgets.QWidget(objectName="page"); f = QtWidgets.QFormLayout(ctl)
        self.sens = QtWidgets.QDoubleSpinBox(); self.sens.setRange(0.5, 12.0); self.sens.setSingleStep(0.5)
        self.sens.setValue(s["mouse_sens"])
        self.inv = QtWidgets.QCheckBox("Fare Y eksenini ters çevir"); self.inv.setChecked(s["mouse_invert"])
        self.gyro = QtWidgets.QDoubleSpinBox(); self.gyro.setRange(1.0, 20.0); self.gyro.setSingleStep(0.5)
        self.gyro.setValue(s["gyro_sens"])
        f.addRow("Fare hassasiyeti", self.sens); f.addRow(self.inv)
        f.addRow("SIXAXIS (Ctrl + fare) hassasiyeti", self.gyro)
        note = QtWidgets.QLabel("Kontrolcü (DualShock, DualSense, Xbox) takılıysa doğrudan çalışır.")
        note.setStyleSheet(f"color: {STEEL};"); f.addRow(note)
        tabs.addTab(ctl, "KONTROLLER")

        adv = QtWidgets.QWidget(objectName="page"); f = QtWidgets.QFormLayout(adv)
        self.gpu = QtWidgets.QComboBox()
        for label, key in (("Otomatik", "auto"), ("Harici ekran kartı", "discrete"), ("Dahili ekran kartı", "integrated")):
            self.gpu.addItem(label, key)
        self.gpu.setCurrentIndex(max(0, self.gpu.findData(s["gpu"])))
        logs = QtWidgets.QPushButton("Günlük klasörünü aç")
        logs.clicked.connect(lambda: QtGui.QDesktopServices.openUrl(QtCore.QUrl.fromLocalFile(str(LOG.parent))))
        reset = QtWidgets.QPushButton("Varsayılanlara dön"); reset.clicked.connect(self.reset)
        f.addRow("Ekran kartı", self.gpu); f.addRow(logs); f.addRow(reset)
        tabs.addTab(adv, "GELİŞMİŞ")

        ok = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Save | QtWidgets.QDialogButtonBox.Cancel)
        ok.button(QtWidgets.QDialogButtonBox.Save).setText("Kaydet")
        ok.button(QtWidgets.QDialogButtonBox.Cancel).setText("Vazgeç")
        ok.accepted.connect(self.accept); ok.rejected.connect(self.reject)
        lay = QtWidgets.QVBoxLayout(self); lay.addWidget(tabs); lay.addWidget(ok)
        self.resize(560, 340)

    def reset(self):
        d = DEFAULTS
        self.full.setChecked(d["fullscreen"]); self.vsync.setCurrentIndex(self.vsync.findData(d["vsync"]))
        self.blur.setChecked(d["post_blur"]); self.sens.setValue(d["mouse_sens"]); self.inv.setChecked(d["mouse_invert"])
        self.gyro.setValue(d["gyro_sens"]); self.gpu.setCurrentIndex(self.gpu.findData(d["gpu"]))

    def result_settings(self):
        return {"fullscreen": self.full.isChecked(), "vsync": self.vsync.currentData(),
                "post_blur": self.blur.isChecked(), "mouse_sens": self.sens.value(),
                "mouse_invert": self.inv.isChecked(), "gyro_sens": self.gyro.value(),
                "gpu": self.gpu.currentData()}


class Launcher(QtWidgets.QWidget):
    def __init__(self):
        super().__init__()
        self.s = load_settings()
        self.proc = None
        self.started = 0.0
        self.setWindowTitle("Killzone 2 — Tokisaki Port")
        icon = GAME / "ICON0.PNG"
        if icon.exists():
            self.setWindowIcon(QtGui.QIcon(str(icon)))
        self.bg = QtGui.QPixmap(str(GAME / "PIC1.PNG")) if (GAME / "PIC1.PNG").exists() else QtGui.QPixmap()
        self.setMinimumSize(1100, 620)

        title = QtWidgets.QLabel("KILLZONE <span style='color:%s'>2</span>" % RED_HOT)
        title.setStyleSheet("font-size: 54pt; font-weight: 800; letter-spacing: 6px;")
        sub = QtWidgets.QLabel("TOKISAKI PORT  ·  PC")
        sub.setStyleSheet(f"color: {STEEL}; font-size: 12pt; letter-spacing: 8px;")

        self.play = QtWidgets.QPushButton("OYNA"); self.play.setObjectName("play")
        self.play.setCursor(QtCore.Qt.PointingHandCursor); self.play.clicked.connect(self.launch)
        settings = QtWidgets.QPushButton("AYARLAR"); settings.clicked.connect(self.open_settings)
        quit_ = QtWidgets.QPushButton("ÇIKIŞ"); quit_.clicked.connect(self.close)
        self.status = QtWidgets.QLabel("● HAZIR"); self.status.setStyleSheet(f"color: {STEEL};")

        ctl = QtWidgets.QGridLayout(); ctl.setHorizontalSpacing(14); ctl.setVerticalSpacing(2)
        for i, (k, v) in enumerate(CONTROLS):
            kl = QtWidgets.QLabel(k); kl.setStyleSheet(f"color: {PALE}; font-weight: 600; font-size: 9pt;")
            vl = QtWidgets.QLabel(v); vl.setStyleSheet(f"color: {STEEL}; font-size: 9pt;")
            ctl.addWidget(kl, i % 5, (i // 5) * 2); ctl.addWidget(vl, i % 5, (i // 5) * 2 + 1)

        left = QtWidgets.QVBoxLayout()
        left.addStretch(2); left.addWidget(title); left.addWidget(sub); left.addSpacing(36)
        row = QtWidgets.QHBoxLayout(); row.addWidget(self.play); row.addSpacing(12)
        row.addWidget(settings); row.addWidget(quit_); row.addStretch(1)
        left.addLayout(row); left.addSpacing(10); left.addWidget(self.status); left.addStretch(3)
        left.addLayout(ctl)
        foot = QtWidgets.QLabel(f"{version()}   ·   ps3recomp ile statik yeniden derleme   ·   "
                                "Killzone 2 © Sony Interactive Entertainment")
        foot.setStyleSheet(f"color: {STEEL}; font-size: 8pt;")
        left.addSpacing(10); left.addWidget(foot)
        lay = QtWidgets.QHBoxLayout(self); lay.setContentsMargins(56, 24, 56, 20)
        lay.addLayout(left); lay.addStretch(1)

        self.timer = QtCore.QTimer(self); self.timer.timeout.connect(self.poll); self.timer.start(500)

    def paintEvent(self, _):
        p = QtGui.QPainter(self)
        p.fillRect(self.rect(), QtGui.QColor(INK))
        if not self.bg.isNull():
            pm = self.bg.scaled(self.size(), QtCore.Qt.KeepAspectRatioByExpanding, QtCore.Qt.SmoothTransformation)
            p.drawPixmap((self.width() - pm.width()) // 2, (self.height() - pm.height()) // 2, pm)
        g = QtGui.QLinearGradient(0, 0, self.width(), 0)
        g.setColorAt(0.0, QtGui.QColor(11, 13, 16, 245)); g.setColorAt(0.45, QtGui.QColor(11, 13, 16, 200))
        g.setColorAt(1.0, QtGui.QColor(11, 13, 16, 40))
        p.fillRect(self.rect(), g)
        p.fillRect(0, self.height() - 3, self.width(), 3, QtGui.QColor(RED))

    def set_status(self, text, color=STEEL):
        self.status.setText(f"● {text}"); self.status.setStyleSheet(f"color: {color};")

    def open_settings(self):
        d = Settings(self.s, self)
        if d.exec_() == QtWidgets.QDialog.Accepted:
            self.s = d.result_settings(); save_settings(self.s); self.set_status("AYARLAR KAYDEDİLDİ")

    def launch(self):
        if self.proc and self.proc.poll() is None:
            return
        if not (ROOT / "build" / "killzone2").exists():
            QtWidgets.QMessageBox.warning(self, "Killzone 2", "Oyun henüz derlenmemiş (build/killzone2 yok).\n"
                                          "KZ2_MAX=1 tools/build.sh ile derleyin.")
            return
        LOG.parent.mkdir(parents=True, exist_ok=True)
        log = open(LOG, "w")
        self.proc = subprocess.Popen([str(ROOT / "tools" / "play.sh")], env=game_env(self.s), cwd=str(ROOT),
                                     stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        self.started = time.time()
        self.play.setEnabled(False); self.set_status("OYUN ÇALIŞIYOR", RED_HOT)
        QtCore.QTimer.singleShot(1500, self.showMinimized)

    def poll(self):
        if not self.proc or self.proc.poll() is None:
            return
        rc, ran = self.proc.returncode, time.time() - self.started
        self.proc = None
        self.play.setEnabled(True); self.showNormal(); self.activateWindow()
        if rc != 0 and ran < 20:
            tail = ""
            try:
                tail = "\n".join(LOG.read_text(errors="replace").splitlines()[-12:])
            except OSError:
                pass
            self.set_status("OYUN BAŞLATILAMADI", RED_HOT)
            QtWidgets.QMessageBox.warning(self, "Killzone 2", f"Oyun kapandı (kod {rc}).\n\n{tail}")
        else:
            self.set_status(f"HAZIR  ·  son oturum {int(ran // 60)} dk")


def main():
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName("Killzone 2"); app.setDesktopFileName("killzone2-tokisaki")
    app.setStyleSheet(QSS)
    w = Launcher(); w.resize(1280, 720); w.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()

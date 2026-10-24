#!/usr/bin/env python3
"""Tokisaki Port -- Killzone 2 launcher.

A small Tk window over the game's own key art: start the game in a window,
or jump straight into the campaign (the menus are walked by PAD_SCRIPT), pick
the GPU and the mouse speed, stop a running game.
  python3 tools/launcher.py
"""
import os
import signal
import subprocess
import tkinter as tk
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = ROOT / "game_dump" / "PS3_GAME"
LOG = ROOT / "logs" / "launcher_game.log"
CAMPAIGN_SCRIPT = "20:0x4000,26:0x4000,32:0x4000,38:0x0008,44:0x4000,50:0x4000,56:0x0008,62:0x4000"

RED = "#c8241c"
RED_HOT = "#ff3b2f"
INK = "#0b0d10"
STEEL = "#8a9099"
PALE = "#e8e2d6"
W, H = 1120, 630


class Launcher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Killzone 2 — Tokisaki Port")
        self.geometry(f"{W}x{H}")
        self.resizable(False, False)
        self.configure(bg=INK)
        self.proc = None
        self.gpu = tk.StringVar(value="igpu")
        self.sens = tk.DoubleVar(value=3.0)
        self.safe = tk.BooleanVar(value=True)
        self.status = tk.StringVar(value="HAZIR")

        icon = GAME / "ICON0.PNG"
        if icon.exists():
            try:
                self.iconphoto(True, tk.PhotoImage(file=str(icon)))
            except tk.TclError:
                pass

        self.c = tk.Canvas(self, width=W, height=H, highlightthickness=0, bg=INK)
        self.c.pack(fill="both", expand=True)
        self._background()
        self._panel()
        self.after(500, self._watch)
        self.protocol("WM_DELETE_WINDOW", self.destroy)

    # --- look -------------------------------------------------------------
    def _background(self):
        art = GAME / "PIC1.PNG"
        self.bg_img = None
        try:
            from PIL import Image, ImageEnhance, ImageFilter, ImageTk
            im = Image.open(art).convert("RGB").resize((W, H), Image.LANCZOS)
            im = ImageEnhance.Brightness(im).enhance(0.55)
            # darken the right third for the menu panel
            shade = Image.new("RGB", (W, H), INK)
            mask = Image.linear_gradient("L").rotate(90).resize((W, H))
            im = Image.composite(shade, im, mask.point(lambda v: int(v * 0.9)))
            self.bg_img = ImageTk.PhotoImage(im)
        except Exception:
            try:
                self.bg_img = tk.PhotoImage(file=str(art)).subsample(2, 2)
            except tk.TclError:
                self.bg_img = None
        if self.bg_img:
            self.c.create_image(0, 0, image=self.bg_img, anchor="nw")
        # scanlines and the red rule
        for y in range(0, H, 4):
            self.c.create_line(0, y, W, y, fill="#000000", stipple="gray12")
        self.c.create_rectangle(0, H - 6, W, H, fill=RED, outline="")
        self.c.create_rectangle(0, 0, W, 3, fill=RED, outline="")

    def _panel(self):
        x = W - 400
        self.c.create_text(x, 70, text="KILLZONE", anchor="w", fill=PALE,
                           font=("DejaVu Sans Condensed", 46, "bold"))
        self.c.create_text(x + 318, 52, text="2", anchor="w", fill=RED_HOT,
                           font=("DejaVu Sans Condensed", 64, "bold"))
        self.c.create_text(x + 2, 112, text="TOKISAKI PORT  ·  PC", anchor="w", fill=STEEL,
                           font=("DejaVu Sans Condensed", 12, "bold"))
        self.c.create_line(x, 132, x + 360, 132, fill=RED, width=2)

        y = 170
        self._button(x, y, "OYNA", "Ana menüden başla", self.play)
        self._button(x, y + 78, "KAMPANYA", "Menüleri otomatik geç, bölüme gir", self.campaign)
        self._button(x, y + 156, "DURDUR", "Çalışan oyunu kapat", self.stop, dim=True)

        # options
        oy = y + 250
        self.c.create_text(x, oy, text="GRAFİK İŞLEMCİ", anchor="w", fill=STEEL,
                           font=("DejaVu Sans Condensed", 10, "bold"))
        for i, (val, label) in enumerate((("igpu", "ENTEGRE (önerilen)"), ("dgpu", "HARİCİ"))):
            rb = tk.Radiobutton(self, text=label, value=val, variable=self.gpu, bg=INK, fg=PALE,
                                selectcolor=INK, activebackground=INK, activeforeground=RED_HOT,
                                highlightthickness=0, bd=0, font=("DejaVu Sans Condensed", 11))
            self.c.create_window(x + i * 190, oy + 26, window=rb, anchor="w")
        self.c.create_text(x, oy + 64, text="FARE HASSASİYETİ", anchor="w", fill=STEEL,
                           font=("DejaVu Sans Condensed", 10, "bold"))
        sc = tk.Scale(self, from_=1, to=8, resolution=0.5, orient="horizontal", variable=self.sens,
                      length=360, bg=INK, fg=PALE, troughcolor="#2a1210", highlightthickness=0,
                      activebackground=RED_HOT, bd=0, sliderrelief="flat",
                      font=("DejaVu Sans Condensed", 9))
        self.c.create_window(x, oy + 94, window=sc, anchor="w")
        cb = tk.Checkbutton(self, text="KARARLI MOD  (son işlem tek SPU'da: bölüm geçişlerinde\ndonmaya karşı; kapalıyken daha hızlı)",
                            variable=self.safe, bg=INK, fg=PALE, selectcolor=INK, justify="left",
                            activebackground=INK, activeforeground=RED_HOT, highlightthickness=0, bd=0,
                            font=("DejaVu Sans Condensed", 10))
        self.c.create_window(x, oy + 140, window=cb, anchor="w")

        self.c.create_text(28, H - 58, text="F1: fareyi bırak  ·  Esc: geri  ·  WASD + fare",
                           anchor="w", fill=STEEL, font=("DejaVu Sans Condensed", 10))
        self.status_id = self.c.create_text(x, H - 30, text="● HAZIR", anchor="w",
                                            fill=STEEL, font=("DejaVu Sans Condensed", 11, "bold"))
        self.c.create_text(28, H - 30, text="CORINTH RIVER  ·  THE NEW SUN", anchor="w",
                           fill="#6f7680", font=("DejaVu Sans Condensed", 11, "bold"))

    def _button(self, x, y, title, sub, cmd, dim=False):
        w, h = 360, 60
        tag = f"btn_{title}"
        edge = "#4a4f57" if dim else RED
        rect = self.c.create_rectangle(x, y, x + w, y + h, fill="#140c0b" if not dim else "#111317",
                                       outline=edge, width=2, tags=tag)
        self.c.create_rectangle(x, y, x + 6, y + h, fill=edge, outline="", tags=tag)
        self.c.create_text(x + 22, y + 22, text=title, anchor="w", fill=PALE,
                           font=("DejaVu Sans Condensed", 18, "bold"), tags=tag)
        self.c.create_text(x + 22, y + 44, text=sub, anchor="w", fill=STEEL,
                           font=("DejaVu Sans Condensed", 9), tags=tag)
        self.c.create_text(x + w - 18, y + h / 2, text="›", anchor="e", fill=edge,
                           font=("DejaVu Sans Condensed", 26, "bold"), tags=tag)

        def enter(_):
            self.c.itemconfig(rect, fill="#3a0e0b" if not dim else "#1d2026", outline=RED_HOT)
            self.config(cursor="hand2")

        def leave(_):
            self.c.itemconfig(rect, fill="#140c0b" if not dim else "#111317", outline=edge)
            self.config(cursor="")

        self.c.tag_bind(tag, "<Enter>", enter)
        self.c.tag_bind(tag, "<Leave>", leave)
        self.c.tag_bind(tag, "<Button-1>", lambda _: cmd())

    def _set_status(self, text, color):
        self.c.itemconfig(self.status_id, text=f"● {text}", fill=color)

    # --- actions ----------------------------------------------------------
    def _running(self):
        return self.proc is not None and self.proc.poll() is None

    def _start(self, campaign):
        if self._running():
            self._set_status("OYUN ZATEN ÇALIŞIYOR", RED_HOT)
            return
        env = dict(os.environ)
        env.pop("PAD_SCRIPT", None)
        env["PS3RECOMP_VK_WINDOW"] = "1"
        env["KZ2_FPS"] = "1"
        env["PS3_PAD_MOUSE_SENS"] = f"{self.sens.get():g}"
        env["PS3RECOMP_VK_PREFER"] = "integrated" if self.gpu.get() == "igpu" else "discrete"
        if self.safe.get():
            env["SPURS_MAXCONT"] = "7:1"     # jm_post on one SPU, as the title builds it
        if campaign:
            env["PAD_SCRIPT"] = CAMPAIGN_SCRIPT
            env["KZ2_RUN_DIR"] = str(ROOT / "run_test")   # the saves the script was made for
        LOG.parent.mkdir(exist_ok=True)
        self.proc = subprocess.Popen([str(ROOT / "tools" / "run.sh")], env=env,
                                     stdout=open(LOG, "wb"), stderr=subprocess.STDOUT,
                                     start_new_session=True)
        self._set_status("KAMPANYA YÜKLENİYOR — İLK 65 SN TUŞLARA DOKUNMA" if campaign
                         else "OYUN BAŞLATILDI", RED_HOT)

    def play(self):
        self._start(False)

    def campaign(self):
        self._start(True)

    def stop(self):
        if not self._running():
            self._set_status("ÇALIŞAN OYUN YOK", STEEL)
            return
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        self._set_status("OYUN KAPATILDI", STEEL)

    def _watch(self):
        if self.proc is not None and self.proc.poll() is not None:
            self.proc = None
            self._set_status("HAZIR", STEEL)
        self.after(1000, self._watch)


if __name__ == "__main__":
    Launcher().mainloop()

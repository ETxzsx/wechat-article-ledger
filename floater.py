# -*- coding: utf-8 -*-
"""
桌面悬浮按钮（推文台账助手）
=================================
常驻桌面最上层的小圆钮；鼠标移上去向左展开「抓取链接 / 退出」。

- 抓取链接：优先用剪贴板里已经复制的公众号链接；没有就自动读前台浏览器的地址栏。
  拿到链接交给本地服务解析，缓存进链接池。
- 退出：收起悬浮按钮，回到网页界面看缓存、批量写入。

用法：pythonw floater.py --port 8770
"""
import argparse
import ctypes
import json
import os
import subprocess
import sys
import threading
import time
import tkinter as tk
import urllib.request

APP_DIR = os.path.dirname(os.path.abspath(__file__))
RUNTIME_PATH = os.path.join(APP_DIR, "data", "runtime.json")

INK = "#1C1E1B"
SEAL = "#9C2B24"
SEAL_DARK = "#7E211B"
PAPER = "#F6F6F1"
RULE = "#C6CABC"

COLLAPSED = 72
EXPANDED = 300
HEIGHT = 72
RADIUS = 33
LOG_PATH = os.path.join(APP_DIR, "data", "floater.log")


def log(msg):
    try:
        os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as fh:
            fh.write("%s  %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg))
    except Exception:
        pass

user32 = ctypes.windll.user32 if os.name == "nt" else None
VK_CONTROL, VK_L, VK_C = 0x11, 0x4C, 0x43
KEYUP = 0x0002


def combo(vk):
    user32.keybd_event(VK_CONTROL, 0, 0, 0)
    time.sleep(0.02)
    user32.keybd_event(vk, 0, 0, 0)
    time.sleep(0.02)
    user32.keybd_event(vk, 0, KEYUP, 0)
    user32.keybd_event(VK_CONTROL, 0, KEYUP, 0)


class Floater:
    def __init__(self, port):
        self.port = port
        self.base = "http://127.0.0.1:%d" % port
        self.expanded = False
        self.dragging = False
        self.pending = 0
        self.busy = False
        self.last_fg = None
        self.idle_job = None
        self.anim_size = COLLAPSED

        if user32:
            try:
                user32.SetProcessDPIAware()
            except Exception:
                pass

        self.root = tk.Tk()
        self.root.overrideredirect(True)
        self.root.attributes("-topmost", True)
        self.root.configure(bg=PAPER)

        self.canvas = tk.Canvas(self.root, width=COLLAPSED, height=HEIGHT, bg=PAPER,
                                highlightthickness=0, bd=0, cursor="hand2")
        self.canvas.pack(fill="both", expand=True)

        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        self.rx = sw - 26                      # 右边缘位置（悬浮按钮贴着右边缘）
        self.y = int(sh * 0.40)
        self.place()
        self.canvas.bind("<Enter>", self.on_enter)
        self.canvas.bind("<Leave>", self.on_leave)
        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<Motion>", self.on_move)
        self.canvas.bind("<Button-3>", lambda e: self.open_web())
        self.root.bind("<Escape>", lambda e: self.collapse())

        self.draw()
        self.root.after(80, self.intro)
        self.root.after(500, self.refresh_badge)

    # ---------------- 布局
    def place(self, width=None):
        w = width or (EXPANDED if self.expanded else COLLAPSED)
        left = self.rx - w
        if left < 6:
            left = 6
        h = HEIGHT if width is None else max(HEIGHT, width)
        size = width or w
        self.root.geometry("%dx%d+%d+%d" % (size, size if width else HEIGHT, left,
                                            self.y - ((size - HEIGHT) // 2 if width else 0)))
        self.canvas.configure(width=size, height=size if width else HEIGHT)

    def intro(self):
        """由大缩到小的入场动作，做出「画面收成一个按钮」的感觉"""
        steps = [128, 112, 98, 86, 78, COLLAPSED]
        for i, s in enumerate(steps):
            self.root.after(i * 36, lambda s=s: self._anim(s))

    def _anim(self, size):
        if self.expanded or size == self.anim_size:
            return
        self.anim_size = size
        if size == COLLAPSED:
            self.place()
            self.draw()
            return
        left = self.rx - size
        top = self.y + HEIGHT // 2 - size // 2
        self.root.geometry("%dx%d+%d+%d" % (size, size, left, top))
        self.canvas.configure(width=size, height=size)

    # ---------------- 网络
    def api(self, path, payload=None, timeout=20):
        try:
            if payload is None:
                req = urllib.request.Request(self.base + path, method="GET")
            else:
                req = urllib.request.Request(self.base + path,
                                             data=json.dumps(payload).encode("utf-8"),
                                             headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads(r.read().decode("utf-8"))
        except Exception as e:
            return {"ok": False, "error": str(e)}

    def refresh_badge(self):
        def work():
            r = self.api("/api/ping", timeout=6)
            if r.get("ok"):
                self.pending = r.get("pending", 0)
                self.root.after(0, self.draw)
            else:
                self.pending = -1          # 服务没起来
                self.root.after(0, self.draw)
        threading.Thread(target=work, daemon=True).start()
        self.root.after(20000, self.refresh_badge)

    # ---------------- 绘制
    def round_rect(self, x1, y1, x2, y2, r, **kw):
        pts = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2,
               x2 - r, y2, x1 + r, y2, x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
        return self.canvas.create_polygon(pts, smooth=True, **kw)

    def draw(self):
        c = self.canvas
        c.delete("all")
        w = EXPANDED if self.expanded else COLLAPSED
        c.configure(width=w, height=HEIGHT)

        if self.expanded:
            self.round_rect(1, 1, w - 1, HEIGHT - 1, 16, fill=PAPER, outline=RULE)
            self.round_rect(9, 13, 168, HEIGHT - 13, 11, fill=SEAL, outline="", tags="grab")
            c.create_text(88, HEIGHT // 2, text="抓取链接", fill="#FBF6F4",
                          font=("Microsoft YaHei UI", 11), tags="grab")
            self.round_rect(176, 13, 228, HEIGHT - 13, 11, fill="#FFFFFF", outline=RULE, tags="quit")
            c.create_text(202, HEIGHT // 2, text="退出", fill=INK,
                          font=("Microsoft YaHei UI", 11), tags="quit")
            # 命中判定统一放到 on_release 里按 x 坐标算，比 tag 命中稳

        cx = w - RADIUS - 3
        cy = HEIGHT // 2
        c.create_oval(cx - RADIUS, cy - RADIUS, cx + RADIUS, cy + RADIUS,
                      fill=INK, outline=INK, tags="ball")
        ring = 4
        c.create_oval(cx - RADIUS + ring, cy - RADIUS + ring, cx + RADIUS - ring, cy + RADIUS - ring,
                      outline="#2E4B3D", width=1, tags="ball")
        c.create_text(cx, cy - 1, text="记", fill=PAPER,
                      font=("Microsoft YaHei UI", 19, "bold"), tags="ball")

        if self.busy:
            c.create_text(cx + 22, cy - 20, text="⋯", fill=SEAL,
                          font=("Microsoft YaHei UI", 14, "bold"))
        elif self.pending > 0:
            bx, by = cx + 21, cy - 21
            c.create_oval(bx - 10, by - 10, bx + 10, by + 10, fill=SEAL, outline=PAPER, width=2)
            c.create_text(bx, by, text=str(min(self.pending, 99)), fill="#FFFFFF",
                          font=("Microsoft YaHei UI", 8, "bold"))
        elif self.pending < 0:
            bx, by = cx + 21, cy - 21
            c.create_oval(bx - 9, by - 9, bx + 9, by + 9, fill=RULE, outline=PAPER, width=2)

    # ---------------- 展开 / 收起
    def expand(self):
        if self.expanded:
            return
        self.expanded = True
        self.place()
        self.draw()

    def collapse(self):
        if not self.expanded:
            return
        self.expanded = False
        self.place()
        self.draw()

    def on_enter(self, _evt=None):
        self.last_fg = user32.GetForegroundWindow() if user32 else None
        if self.idle_job:
            self.root.after_cancel(self.idle_job)
            self.idle_job = None
        self.expand()

    def on_leave(self, _evt=None):
        if self.idle_job:
            self.root.after_cancel(self.idle_job)
        self.idle_job = self.root.after(650, self._maybe_collapse)

    def _maybe_collapse(self):
        self.idle_job = None
        px, py = self.root.winfo_pointerxy()
        rx, ry = self.root.winfo_rootx(), self.root.winfo_rooty()
        if rx <= px <= rx + self.root.winfo_width() and ry <= py <= ry + self.root.winfo_height():
            return
        self.collapse()

    # ---------------- 拖动
    def on_move(self, evt):
        if not self.expanded:
            return
        self.canvas.configure(cursor="hand2" if evt.x < 232 else "arrow")

    def on_press(self, evt):
        self.dragging = True
        self.moved = False
        self._start_rx = self.rx
        self._start_y = self.y
        self._start_xy = (evt.x_root, evt.y_root)
        log("press x=%s expanded=%s geo=%sx%s+%s+%s rx=%s" % (
            evt.x, self.expanded, self.root.winfo_width(), self.root.winfo_height(),
            self.root.winfo_rootx(), self.root.winfo_rooty(), self.rx))

    def on_drag(self, evt):
        if not self.dragging:
            return
        dx = evt.x_root - self._start_xy[0]
        dy = evt.y_root - self._start_xy[1]
        if abs(dx) > 3 or abs(dy) > 3:
            self.moved = True
        self.rx = max(EXPANDED + 8, self._start_rx + dx) if self.expanded else max(COLLAPSED + 8, self._start_rx + dx)
        self.y = max(8, min(self.root.winfo_screenheight() - HEIGHT - 8, self._start_y + dy))
        self.place()

    def on_release(self, evt):
        was_drag = getattr(self, "moved", False)
        self.dragging = False
        if was_drag:
            return
        log("release x=%s y=%s expanded=%s" % (evt.x, evt.y, self.expanded))
        if not self.expanded:
            self.expand()
            return
        if evt.x < 172:
            self.on_grab()
        elif evt.x < 236:
            self.on_quit()

    # ---------------- 剪贴板 / 抓取
    def read_clip(self):
        try:
            return self.root.clipboard_get()
        except Exception:
            return ""

    def write_clip(self, text):
        try:
            self.root.clipboard_clear()
            if text:
                self.root.clipboard_append(text)
            self.root.update()
        except Exception:
            pass

    def is_weixin(self, text):
        return "mp.weixin.qq.com" in (text or "")

    def grab_url(self):
        prev = self.read_clip()
        if self.is_weixin(prev):
            return prev
        if user32 and self.last_fg:
            try:
                user32.SetForegroundWindow(self.last_fg)
                time.sleep(0.2)
                self.write_clip("")
                combo(VK_L)
                time.sleep(0.2)
                combo(VK_C)
                time.sleep(0.4)
                got = self.read_clip()
                self.write_clip(prev)
                if self.is_weixin(got):
                    return got
            except Exception:
                pass
        return ""

    def on_grab(self, _evt=None):
        log("grab clicked")
        if self.busy:
            return
        self.busy = True
        self.draw()
        threading.Thread(target=self._grab_worker, daemon=True).start()

    def _grab_worker(self):
        url = self.grab_url()
        if not url:
            self.root.after(0, lambda: self.toast("没读到链接：先在浏览器打开推文，或复制链接再点", err=True))
            self.root.after(0, self._done)
            return
        r = self.api("/api/capture", {"url": url, "source": "floating"}, timeout=45)
        if r.get("ok"):
            item = r.get("item") or {}
            title = item.get("title") or "（没读到标题）"
            short = title if len(title) <= 22 else title[:22] + "…"
            self.pending = r.get("pending", self.pending)
            msg = ("已缓存：《%s》" % short) if r.get("new") else ("已在池中：《%s》" % short)
            self.root.after(0, lambda: self.toast(msg))
        else:
            err = str(r.get("error") or "抓取失败")
            self.root.after(0, lambda: self.toast("抓取失败：" + err[:36], err=True))
        self.root.after(0, self._done)

    def _done(self):
        self.busy = False
        self.draw()

    def on_quit(self, _evt=None):
        log("quit clicked")
        self.api("/api/floating/stop", {}, timeout=8)
        self.open_web()
        self.root.after(120, self.root.destroy)

    def open_web(self):
        try:
            import webbrowser
            webbrowser.open(self.base + "/")
        except Exception:
            pass

    # ---------------- 提示条
    def toast(self, text, err=False):
        try:
            top = tk.Toplevel(self.root)
            top.overrideredirect(True)
            top.attributes("-topmost", True)
            bg = SEAL_DARK if err else INK
            tk.Label(top, text=text, bg=bg, fg=PAPER, padx=15, pady=10,
                     font=("Microsoft YaHei UI", 10)).pack()
            top.update_idletasks()
            tw, th = top.winfo_width(), top.winfo_height()
            tx = max(8, self.rx - tw)
            ty = max(8, self.y - th - 10)
            top.geometry("+%d+%d" % (tx, ty))
            top.after(3000, top.destroy)
        except Exception:
            pass

    def run(self):
        self.root.mainloop()


def ping(port, timeout=1.5):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/api/ping" % port, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8")).get("ok", False)
    except Exception:
        return False


def ensure_server(port):
    """悬浮按钮自己把服务带起来，用户双击一个文件就够"""
    if ping(port):
        return True
    py = os.path.join(os.path.dirname(sys.executable), "python.exe")
    if not os.path.exists(py):
        py = sys.executable
    script = os.path.join(APP_DIR, "server.py")
    if not os.path.exists(script):
        return False
    flags = 0x00000008 if os.name == "nt" else 0
    try:
        subprocess.Popen([py, "-X", "utf8", script, "--no-browser"], cwd=APP_DIR,
                         creationflags=flags, close_fds=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        return False
    for _ in range(40):
        time.sleep(0.4)
        if ping(port):
            return True
    return False


def read_port(default=8770):
    try:
        with open(RUNTIME_PATH, "r", encoding="utf-8") as fh:
            return int(json.load(fh).get("port") or default)
    except Exception:
        return default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=0)
    args = ap.parse_args()
    port = args.port or read_port()
    if not ensure_server(port):
        try:
            import tkinter.messagebox as mb
            r = tk.Tk(); r.withdraw()
            mb.showerror("推文台账助手", "本地服务没能起来，请先双击「启动.cmd」。")
        except Exception:
            pass
        return
    # 把进程号登记给服务，网页上的「退出悬浮模式」才知道关谁
    try:
        req = urllib.request.Request("http://127.0.0.1:%d/api/floating/hello" % port,
                                     data=json.dumps({"pid": os.getpid()}).encode("utf-8"),
                                     headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=6).read()
    except Exception:
        pass
    Floater(port).run()


if __name__ == "__main__":
    main()

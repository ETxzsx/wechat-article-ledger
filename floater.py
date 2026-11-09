# -*- coding: utf-8 -*-
"""
桌面悬浮按钮（推文台账助手）
=================================
常驻桌面最上层的小圆钮；鼠标移上去向左展开「抓取链接 / 退出」。

抓链接的顺序（从快到慢，哪一步成都立刻返回）：
1. 剪贴板里已经有 mp.weixin.qq.com 的文章链接 -> 直接用（微信里「复制链接」就是这么走的）
2. 前台是浏览器 -> 强制把浏览器提到前面，Ctrl+L 选中地址栏、Ctrl+C，读剪贴板
3. 还不行 -> 按前台窗口标题去浏览器历史里找最近访问的那篇文章（标题比对）
4. 还不行 -> 进入「等你复制」模式：15 秒内你复制了链接就自动抓
5. 还没中 -> 弹输入框，手动粘一次

用法：pythonw floater.py --port 8770
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import glob
import json
import os
import re
import sqlite3
import subprocess
import sys
import threading
import time
import tkinter as tk
import urllib.request
from difflib import SequenceMatcher
from urllib.parse import urlparse, parse_qs

APP_DIR = os.path.dirname(os.path.abspath(__file__))
RUNTIME_PATH = os.path.join(APP_DIR, "data", "runtime.json")
LOG_PATH = os.path.join(APP_DIR, "data", "floater.log")

INK = "#1C1E1B"
SEAL = "#9C2B24"
SEAL_DARK = "#7E211B"
PAPER = "#F6F6F1"
RULE = "#C6CABC"

COLLAPSED = 72
EXPANDED = 300
HEIGHT = 72
RADIUS = 33

user32 = ctypes.windll.user32 if os.name == "nt" else None
kernel32 = ctypes.windll.kernel32 if os.name == "nt" else None

if os.name == "nt":
    # 64 位下不声明这些，句柄会被截成 int32，剪贴板永远读不到
    kernel32.GlobalAlloc.argtypes = [ctypes.c_uint, ctypes.c_size_t]
    kernel32.GlobalAlloc.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    kernel32.GlobalUnlock.restype = ctypes.c_int
    kernel32.OpenProcess.argtypes = [ctypes.c_uint, ctypes.c_int, ctypes.c_uint]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                                    ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)]
    kernel32.QueryFullProcessImageNameW.restype = ctypes.c_int
    user32.OpenClipboard.argtypes = [ctypes.c_void_p]
    user32.OpenClipboard.restype = ctypes.c_int
    user32.CloseClipboard.restype = ctypes.c_int
    user32.EmptyClipboard.restype = ctypes.c_int
    user32.GetClipboardData.argtypes = [ctypes.c_uint]
    user32.GetClipboardData.restype = ctypes.c_void_p
    user32.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
    user32.SetClipboardData.restype = ctypes.c_void_p
    user32.GetForegroundWindow.restype = ctypes.c_void_p
    user32.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
    user32.GetWindowTextLengthW.restype = ctypes.c_int
    user32.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    user32.GetWindowTextW.restype = ctypes.c_int
    user32.GetClipboardSequenceNumber.argtypes = []
    user32.GetClipboardSequenceNumber.restype = ctypes.c_uint
    user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
    user32.GetWindowThreadProcessId.restype = ctypes.c_ulong
    user32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
    user32.SetForegroundWindow.restype = ctypes.c_int
    user32.BringWindowToTop.argtypes = [ctypes.c_void_p]
    user32.AttachThreadInput.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int]
VK_CONTROL, VK_L, VK_C, VK_MENU = 0x11, 0x4C, 0x43, 0x12
KEYUP = 0x0002
CF_UNICODETEXT = 13

BROWSERS = {
    "chrome.exe", "msedge.exe", "firefox.exe", "brave.exe", "opera.exe", "vivaldi.exe",
    "360se.exe", "360chrome.exe", "360se6.exe", "qqbrowser.exe", "sogouexplorer.exe",
    "iexplore.exe", "chromium.exe", "centbrowser.exe",
}
WECHAT = {"wechat.exe", "weixin.exe", "wechatappex.exe", "wechatwin.exe"}

TITLE_SUFFIXES = [
    " - google chrome", " - microsoft edge", " - chrome", " - edge", " - chromium",
    " - 360安全浏览器", " - 360极速浏览器", " - 360se", " - qq浏览器", " - 搜狗高速浏览器",
    " - firefox", " — mozilla firefox", " - mozilla firefox", " - opera", " - 微信",
]

LINK_RE = re.compile(r"(?:https?://)?mp\.weixin\.qq\.com/[^\s\"'<>，。；、）)】\]]+")


def log(msg):
    try:
        os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as fh:
            fh.write("%s  %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg))
    except Exception:
        pass


# ---------------------------------------------------------------- Win32 小工具

def win_title(hwnd):
    try:
        n = user32.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return ""
        buf = ctypes.create_unicode_buffer(n + 2)
        user32.GetWindowTextW(hwnd, buf, n + 2)
        return buf.value
    except Exception:
        return ""


def win_proc(hwnd):
    try:
        pid = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        h = kernel32.OpenProcess(0x1000, False, pid.value)
        if not h:
            return ""
        try:
            buf = ctypes.create_unicode_buffer(1024)
            size = ctypes.c_ulong(1024)
            if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
                return os.path.basename(buf.value).lower()
        finally:
            kernel32.CloseHandle(h)
    except Exception:
        pass
    return ""


GA_ROOT = 2


def root_window(hwnd):
    try:
        r = user32.GetAncestor(hwnd, GA_ROOT)
        return r or hwnd
    except Exception:
        return hwnd


def force_foreground(hwnd):
    """Windows 有前台锁，常规做法（AttachThreadInput + 一次 ALT）经常还不够"""
    try:
        hwnd = root_window(hwnd)
        if user32.GetForegroundWindow() == hwnd:
            return True
        try:
            user32.AllowSetForegroundWindow(-1)      # ASFW_ANY
        except Exception:
            pass
        fg = user32.GetForegroundWindow()
        tid_fg = user32.GetWindowThreadProcessId(fg, None)
        tid_tg = user32.GetWindowThreadProcessId(hwnd, None)
        user32.keybd_event(VK_MENU, 0, 0, 0)
        user32.keybd_event(VK_MENU, 0, KEYUP, 0)
        if tid_fg and tid_tg and tid_fg != tid_tg:
            user32.AttachThreadInput(tid_fg, tid_tg, True)
            user32.BringWindowToTop(hwnd)
            user32.SetForegroundWindow(hwnd)
            user32.AttachThreadInput(tid_fg, tid_tg, False)
        else:
            user32.BringWindowToTop(hwnd)
            user32.SetForegroundWindow(hwnd)
        time.sleep(0.2)
        if user32.GetForegroundWindow() != hwnd:
            try:
                user32.SwitchToThisWindow(hwnd, True)
                time.sleep(0.2)
            except Exception:
                pass
        ok = user32.GetForegroundWindow() == hwnd
        log("force_foreground %s -> %s" % (hwnd, ok))
        return ok
    except Exception as e:
        log("force_foreground failed: %s" % e)
        return False


def combo(vk):
    user32.keybd_event(VK_CONTROL, 0, 0, 0)
    time.sleep(0.02)
    user32.keybd_event(vk, 0, 0, 0)
    time.sleep(0.02)
    user32.keybd_event(vk, 0, KEYUP, 0)
    user32.keybd_event(VK_CONTROL, 0, KEYUP, 0)


def clip_seq():
    """剪贴板版本号：变了说明用户刚复制过"""
    try:
        return int(user32.GetClipboardSequenceNumber())
    except Exception:
        return 0


def same_article_key(a, b):
    try:
        ua, ub = urlparse(a), urlparse(b)
        if ua.path and ua.path == ub.path:
            return True
        return url_key(a) == url_key(b)
    except Exception:
        return a == b


def history_title_for(url):
    """本地浏览器历史里，这条链接的标题（很快，用来核对）"""
    appdata = os.environ.get("LOCALAPPDATA", "")
    patterns = [
        os.path.join(appdata, "Google", "Chrome", "User Data", "*", "History"),
        os.path.join(appdata, "Microsoft", "Edge", "User Data", "*", "History"),
        os.path.join(appdata, "BraveSoftware", "Brave-Browser", "User Data", "*", "History"),
    ]
    for pat in patterns:
        for path in glob.glob(pat):
            try:
                uri = "file:%s?mode=ro&immutable=1" % path.replace("\\", "/")
                con = sqlite3.connect(uri, uri=True, timeout=2)
                rows = con.execute(
                    "SELECT url, title FROM urls WHERE url LIKE '%mp.weixin.qq.com%' "
                    "ORDER BY last_visit_time DESC LIMIT 300").fetchall()
                con.close()
            except Exception:
                continue
            for u, t in rows:
                if t and same_article_key(u, url):
                    return t
    return ""


def clip_get(max_try=5):
    """直接用 Win32 读剪贴板，避免 tkinter 在子线程里操作 Tcl"""
    for _ in range(max_try):
        try:
            if not user32.OpenClipboard(None):
                time.sleep(0.06)
                continue
            try:
                h = user32.GetClipboardData(CF_UNICODETEXT)
                if not h:
                    return ""
                ptr = kernel32.GlobalLock(h)
                if not ptr:
                    return ""
                try:
                    return ctypes.wstring_at(ptr)
                finally:
                    kernel32.GlobalUnlock(h)
            finally:
                user32.CloseClipboard()
        except Exception:
            time.sleep(0.06)
    return ""


def clip_set(text):
    data = ctypes.create_unicode_buffer(text)
    for _ in range(5):
        try:
            if not user32.OpenClipboard(None):
                time.sleep(0.06)
                continue
            try:
                user32.EmptyClipboard()
                size = len(text) * 2 + 2
                h = kernel32.GlobalAlloc(0x0002, size)
                if not h:
                    return False
                ptr = kernel32.GlobalLock(h)
                ctypes.memmove(ptr, data, size)
                kernel32.GlobalUnlock(h)
                user32.SetClipboardData(CF_UNICODETEXT, h)
                return True
            finally:
                user32.CloseClipboard()
        except Exception:
            time.sleep(0.06)
    return False


# ---------------------------------------------------------------- 链接判定

def norm_title(t):
    """窗口标题归一：零宽字符 / 「和另外 N 个页面」/ 「 - 个人」这类 profile 尾巴 / 浏览器名"""
    t = (t or "").strip()
    for ch in ("\u200b", "\u200c", "\u200d", "\ufeff", "\u2060"):
        t = t.replace(ch, "")
    low = t.lower()
    for s in TITLE_SUFFIXES:
        if low.endswith(s):
            t = t[: len(t) - len(s)]
            low = t.lower()
            break
    for _ in range(3):
        before = t
        t = re.sub(r"\s*[-—|]?\s*和另外\s*\d+\s*个页面\s*$", "", t)
        t = re.sub(r"\s*(?:[-—]\s*)?(?:and\s*)?\d+\s*more\s*pages?\s*$", "", t, flags=re.I)
        m = re.search(r"\s[-—]\s([^-—]{1,8})$", t)
        if m and len(t) - len(m.group(0)) >= 8:
            t = t[: len(t) - len(m.group(0))]
        if t == before:
            break
    return re.sub(r"[\s\W_]+", "", t)


def is_article(url):
    """是不是「一篇推文」，而不是公众号主页、合集、搜索页"""
    if not url or "mp.weixin.qq.com" not in url:
        return False
    try:
        u = urlparse(url if url.startswith("http") else "https://" + url)
    except Exception:
        return False
    if not u.netloc.endswith("mp.weixin.qq.com"):
        return False
    path = u.path or "/"
    if path.startswith("/s/"):
        return len(path) > 4
    if path in ("/s", "/s/"):
        q = parse_qs(u.query)
        return "__biz" in q or "mid" in q or "sn" in q
    return False


def url_key(url):
    try:
        u = urlparse(url)
        if u.path.startswith("/s/"):
            return u.path.rstrip("/").lower()
        return ((u.path or "/") + "?" + u.query).lower()
    except Exception:
        return url


def extract_article(text):
    for raw in LINK_RE.findall(text or ""):
        url = raw.rstrip(".,;:)]}\"'")
        if not url.startswith("http"):
            url = "https://" + url
        if is_article(url):
            return url
    return ""


def history_lookup(title):
    """按窗口标题去浏览器历史里找那篇文章（浏览器拿不到地址栏时的兜底）"""
    base = norm_title(title)
    if len(base) < 6:
        return ""
    best = None
    appdata = os.environ.get("LOCALAPPDATA", "")
    patterns = [
        os.path.join(appdata, "Google", "Chrome", "User Data", "*", "History"),
        os.path.join(appdata, "Microsoft", "Edge", "User Data", "*", "History"),
        os.path.join(appdata, "BraveSoftware", "Brave-Browser", "User Data", "*", "History"),
        os.path.join(appdata, "360se6", "User Data", "*", "History"),
        os.path.join(appdata, "Tencent", "QQBrowser", "User Data", "*", "History"),
    ]
    for pat in patterns:
        for path in glob.glob(pat):
            try:
                uri = "file:%s?mode=ro&immutable=1" % path.replace("\\", "/")
                con = sqlite3.connect(uri, uri=True, timeout=2)
                rows = con.execute(
                    "SELECT url, title, last_visit_time FROM urls "
                    "WHERE url LIKE '%mp.weixin.qq.com%' "
                    "ORDER BY last_visit_time DESC LIMIT 80").fetchall()
                con.close()
            except Exception:
                continue
            for url, hit_title, lvt in rows:
                if not is_article(url) or not hit_title:
                    continue
                sim = SequenceMatcher(None, base, norm_title(hit_title)).ratio()
                if sim < 0.72:
                    continue
                cand = (round(sim, 2), lvt or 0, url, hit_title)
                if best is None or cand[:2] > best[:2]:
                    best = cand
    if best:
        log("history hit %.2f %s" % (best[0], best[2]))
        return best[2]
    return ""


# ---------------------------------------------------------------- 悬浮窗

class Floater:
    def __init__(self, port):
        self.port = port
        self.base = "http://127.0.0.1:%d" % port
        self.expanded = False
        self.dragging = False
        self.pending = 0
        self.busy = False
        self.mode = ""            # "" | "busy" | "waiting"
        self.fg_hwnd = None
        self.fg_title = ""
        self.fg_proc = ""
        self.fg_ts = 0.0
        self.clip_seq_seen = 0
        self.clip_seq_ts = 0.0
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
        self.rx = sw - 26
        self.y = int(sh * 0.40)
        self.place()

        self.canvas.bind("<Enter>", self.on_enter)
        self.canvas.bind("<Leave>", self.on_leave)
        self.canvas.bind("<ButtonPress-1>", self.on_press)
        self.canvas.bind("<B1-Motion>", self.on_drag)
        self.canvas.bind("<ButtonRelease-1>", self.on_release)
        self.canvas.bind("<Button-3>", lambda e: self.open_web())
        self.root.bind("<Escape>", lambda e: self.collapse())

        self.draw()
        self.root.after(80, self.intro)
        self.root.after(500, self.refresh_badge)
        self.track_foreground()

    # ---------------- 布局
    def place(self, width=None):
        w = width or (EXPANDED if self.expanded else COLLAPSED)
        left = self.rx - w
        if left < 6:
            left = 6
        if width:
            top = self.y + HEIGHT // 2 - max(HEIGHT, width) // 2
            self.root.geometry("%dx%d+%d+%d" % (width, max(HEIGHT, width), left, top))
            self.canvas.configure(width=width, height=max(HEIGHT, width))
        else:
            self.root.geometry("%dx%d+%d+%d" % (w, HEIGHT, left, self.y))
            self.canvas.configure(width=w, height=HEIGHT)

    def intro(self):
        for i, s in enumerate([128, 112, 98, 86, 78, COLLAPSED]):
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
            self.pending = r.get("pending", 0) if r.get("ok") else -1
            self.root.after(0, self.draw)
        threading.Thread(target=work, daemon=True).start()
        self.root.after(20000, self.refresh_badge)

    def track_foreground(self):
        """持续记住「用户正在看的那个窗口」和「剪贴板多久前变过」"""
        try:
            sq = clip_seq()
            if sq != self.clip_seq_seen:
                self.clip_seq_seen = sq
                self.clip_seq_ts = time.time()
        except Exception:
            pass
        try:
            hwnd = user32.GetForegroundWindow()
            if hwnd:
                pid = ctypes.c_ulong()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value and pid.value != os.getpid():
                    title = win_title(hwnd)
                    if title:
                        proc = win_proc(hwnd)
                        if proc != self.fg_proc or title != self.fg_title:
                            log("fg -> %s | %s" % (proc, title[:50]))
                        self.fg_hwnd = hwnd
                        self.fg_title = title
                        self.fg_proc = proc
                        self.fg_ts = time.time()
        except Exception:
            pass
        self.root.after(700, self.track_foreground)

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
            label = "等你复制…" if self.mode == "waiting" else ("抓取中…" if self.mode == "busy" else "抓取链接")
            self.round_rect(9, 13, 168, HEIGHT - 13, 11,
                            fill=SEAL if self.mode != "waiting" else "#B4574D", outline="", tags="grab")
            c.create_text(88, HEIGHT // 2, text=label, fill="#FBF6F4",
                          font=("Microsoft YaHei UI", 11), tags="grab")
            self.round_rect(176, 13, 228, HEIGHT - 13, 11, fill="#FFFFFF", outline=RULE, tags="quit")
            c.create_text(202, HEIGHT // 2, text="退出", fill=INK,
                          font=("Microsoft YaHei UI", 11), tags="quit")

        cx = w - RADIUS - 3
        cy = HEIGHT // 2
        c.create_oval(cx - RADIUS, cy - RADIUS, cx + RADIUS, cy + RADIUS,
                      fill=INK, outline=INK, tags="ball")
        c.create_oval(cx - RADIUS + 4, cy - RADIUS + 4, cx + RADIUS - 4, cy + RADIUS - 4,
                      outline="#2E4B3D", width=1, tags="ball")
        c.create_text(cx, cy - 1, text="记", fill=PAPER,
                      font=("Microsoft YaHei UI", 19, "bold"), tags="ball")

        if self.mode == "busy" or self.mode == "waiting":
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

    # ---------------- 拖动 / 点击
    def on_move(self, evt):
        pass

    def on_press(self, evt):
        self.dragging = True
        self.moved = False
        self._start_rx = self.rx
        self._start_y = self.y
        self._start_xy = (evt.x_root, evt.y_root)

    def on_drag(self, evt):
        if not self.dragging:
            return
        dx = evt.x_root - self._start_xy[0]
        dy = evt.y_root - self._start_xy[1]
        if abs(dx) > 3 or abs(dy) > 3:
            self.moved = True
        self.rx = max(COLLAPSED + 8, self._start_rx + dx)
        self.y = max(8, min(self.root.winfo_screenheight() - HEIGHT - 8, self._start_y + dy))
        self.place()

    def on_release(self, evt):
        was_drag = getattr(self, "moved", False)
        self.dragging = False
        if was_drag:
            return
        if not self.expanded:
            self.expand()
            return
        if evt.x < 172:
            self.on_grab()
        elif evt.x < 236:
            self.on_quit()

    # ---------------- 抓取
    def on_grab(self):
        if self.mode == "busy":
            return
        self.mode = "busy"
        self.draw()
        log("grab: fg=%s title=%r" % (self.fg_proc, self.fg_title[:60]))
        threading.Thread(target=self._grab_worker, daemon=True).start()

    def _set_mode(self, mode):
        self.mode = mode
        self.root.after(0, self.draw)

    def _grab_worker(self):
        try:
            url, how = self.find_url()
        except Exception as e:
            log("find_url failed: %r" % (e,))
            url, how = "", ""
        if not url:
            self._set_mode("")
            self.root.after(0, lambda: self.ask_manual())
            return
        self._capture(url, how)

    def title_ok(self, url, label=""):
        """剪贴板/历史里这条，和用户正看着的那篇是不是同一篇"""
        win = norm_title(self.fg_title)
        if len(win) < 6:
            return True, ""
        known = history_title_for(url)
        got = norm_title(known)
        if not got:
            r = self.api("/api/peek", {"url": url}, timeout=25)
            got = norm_title(r.get("title") or "") if r.get("ok") else ""
        if not got:
            log("title check %s: 拿不到标题，放行" % label)
            return True, known
        sim = SequenceMatcher(None, got, win).ratio()
        log("title check %s: %r vs %r -> %.2f" % (label, got[:18], win[:18], sim))
        return sim >= 0.55, known

    def find_url(self):
        stale_url, stale_title = "", ""

        # 1) 剪贴板：要么是「刚复制的」，要么得和当前窗口标题对得上，否则就是上次剩下的旧链接
        clip = clip_get()
        seq = clip_seq()
        hit = extract_article(clip)
        if hit:
            fresh_copy = (time.time() - self.clip_seq_ts) <= 15
            if fresh_copy:
                log("clipboard hit（刚复制过）")
                return hit, "clipboard"
            ok, known = self.title_ok(hit, "clipboard")
            if ok:
                log("clipboard hit（标题核对通过）")
                return hit, "clipboard"
            stale_url, stale_title = hit, known
            log("剪贴板里是旧链接，跳过：%s" % hit)

        # 前台窗口超过 25 秒没更新，就当它已经过期，别去抢人家的焦点
        fresh = (time.time() - self.fg_ts) <= 25

        # 2) 前台是浏览器：抢前景 + Ctrl+L / Ctrl+C 读地址栏
        address_url = ""
        if fresh and self.fg_proc in BROWSERS and self.fg_hwnd:
            ok = force_foreground(self.fg_hwnd)
            log("browser %s foreground=%s" % (self.fg_proc, ok))
            try:
                combo(VK_L)
                time.sleep(0.18)
                combo(VK_C)
                time.sleep(0.45)
            except Exception as e:
                log("key send failed: %s" % e)
            address_url = extract_article(clip_get())
            log("addressbar -> %r" % (address_url or "空"))

        # 3) 用窗口标题去浏览器历史里找（标题才是「用户正在看哪篇」的硬证据）
        hist_url = history_lookup(self.fg_title) if (fresh and self.fg_title) else ""

        if hist_url and address_url and url_key(hist_url) != url_key(address_url):
            log("addressbar=%s 与 history=%s 不一致，按标题取 history" % (address_url, hist_url))
            return hist_url, "history"
        if hist_url:
            return hist_url, "history"
        if address_url:
            return address_url, "addressbar"

        # 4) 等你复制（微信客户端里的文章只能这么拿：复制链接）
        log("fall through to waiting mode, fg=%s fresh=%s stale=%s"
            % (self.fg_proc, fresh, bool(stale_url)))
        self._set_mode("waiting")
        self.root.after(0, lambda: self.toast(self.wait_hint(stale_title)))
        end = time.time() + 22
        while time.time() < end:
            time.sleep(0.3)
            got = clip_get()
            hit = extract_article(got)
            if not hit:
                continue
            recent = (time.time() - self.clip_seq_ts) <= 15
            if recent or self.title_ok(hit, "wait")[0]:
                log("clipboard during wait（recent=%s）" % recent)
                return hit, "waited"

        # 5) 手动
        self._set_mode("")
        return "", ""

    def wait_hint(self, stale_title=""):
        if stale_title:
            short = stale_title if len(stale_title) <= 18 else stale_title[:18] + "…"
            return "剪贴板里是《%s》，不是你正在看的这篇。复制当前文章链接，我在等（22 秒）" % short
        if self.fg_proc in WECHAT:
            return "微信里的文章：点右上角 ⋯ →「复制链接」，我在等（22 秒）"
        return "没读到链接：复制一下这篇文章的链接，我在等（22 秒）"

    def ask_manual(self):
        """兜底：自己弹一个小输入框，比 simpledialog 更好控置顶和焦点"""
        self.toast("没拿到链接，粘贴一次吧", err=True)
        top = tk.Toplevel(self.root)
        top.overrideredirect(True)
        top.attributes("-topmost", True)
        top.configure(bg=PAPER)
        w, h = 430, 176
        x = max(8, self.rx - w)
        y = max(8, self.y - h - 100)
        top.geometry("%dx%d+%d+%d" % (w, h, x, y))

        win = (self.fg_title or "").strip()
        win = re.sub(r"\s*[-—]\s*[^-—]{1,12}$", "", win)[:46]
        tip = "没读到当前页面的链接。"
        if win:
            tip += "\n（我认到的前台窗口是：《%s》）" % win
        tk.Label(top, text=tip + "\n把文章链接粘贴到这里：",
                 bg=PAPER, fg=INK, font=("Microsoft YaHei UI", 9),
                 wraplength=w - 28, justify="left").pack(anchor="w", padx=14, pady=(10, 4))
        var = tk.StringVar(value=(clip_get() or "").strip()[:400])
        ent = tk.Entry(top, textvariable=var, font=("Microsoft YaHei UI", 10),
                       bg="#FFFFFF", fg=INK, relief="solid", bd=1)
        ent.pack(padx=14, fill="x", ipady=4)
        ent.focus_force()
        ent.select_range(0, "end")

        row = tk.Frame(top, bg=PAPER)
        row.pack(fill="x", padx=14, pady=10)

        def do_ok(_evt=None):
            val = var.get().strip()
            top.destroy()
            url = extract_article(val) or (val if is_article(val) else "")
            if url:
                self._set_mode("busy")
                threading.Thread(target=self._capture, args=(url, "manual"), daemon=True).start()
            else:
                self._set_mode("")
                self.toast("这也不是文章链接，再复制一次试试", err=True)

        def do_cancel(_evt=None):
            top.destroy()
            self._set_mode("")

        for text, cmd, primary in (("取消", do_cancel, False), ("抓这一篇", do_ok, True)):
            b = tk.Button(row, text=text, command=cmd, font=("Microsoft YaHei UI", 10),
                          bg=SEAL if primary else "#FFFFFF", fg="#FFFFFF" if primary else INK,
                          relief="flat", padx=14, pady=4,
                          activebackground=SEAL_DARK if primary else "#EEEEE8",
                          activeforeground="#FFFFFF" if primary else INK)
            b.pack(side="right", padx=(8, 0))
        top.bind("<Return>", do_ok)
        top.bind("<Escape>", do_cancel)

    def _capture(self, url, how):
        self._set_mode("busy")
        r = self.api("/api/capture", {"url": url, "source": "floating", "title_hint": self.fg_title},
                     timeout=45)
        self.root.after(0, lambda: self._after_capture(r, url, how))

    def _after_capture(self, r, url, how):
        self.mode = ""
        if not r.get("ok"):
            err = str(r.get("error") or "抓取失败")
            log("capture failed: %s" % err)
            self.toast("抓取失败：" + err[:38], err=True)
            self.draw()
            return
        item = r.get("item") or {}
        title = item.get("title") or "（没读到标题）"
        short = title if len(title) <= 22 else title[:22] + "…"
        self.pending = r.get("pending", self.pending)
        msg = ("已缓存：《%s》" % short) if r.get("new") else ("已在池中：《%s》" % short)
        if r.get("mismatch"):
            msg = "缓存了《%s》，但和你当前页面标题对不上，看一眼对不对" % short
        elif item.get("status") != "parsed":
            msg = "链接已缓存，但正文没解析出来（可能遇到微信验证页），去链接池里手填"
        self.toast(msg)
        self.draw()

    def on_quit(self):
        log("quit")
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
            wrap = 360
            tk.Label(top, text=text, bg=bg, fg=PAPER, padx=15, pady=10, wraplength=wrap,
                     justify="left", font=("Microsoft YaHei UI", 10)).pack()
            top.update_idletasks()
            tw, th = top.winfo_width(), top.winfo_height()
            tx = max(8, self.rx - tw)
            ty = max(8, self.y - th - 10)
            top.geometry("+%d+%d" % (tx, ty))
            top.after(3400, top.destroy)
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

# -*- coding: utf-8 -*-
"""
推文台账助手 · 本地服务端
=================================
微信公众号文章链接 -> 解析（标题/公众号/发布时间/署名）-> 链接池 -> 追加到 Excel 台账
另带：批量导入（粘贴 / 表格文件）、批量写入、桌面悬浮按钮的启动与停止。

只依赖标准库 + openpyxl。
"""
import io
import json
import os
import re
import sys
import csv
import html as htmlmod
import time
import uuid
import shutil
import stat
import threading
import datetime
import mimetypes
import traceback
import subprocess
import urllib.request
import urllib.error
from copy import copy
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, unquote

from openpyxl import load_workbook
from openpyxl.styles import Font, Alignment, Border
from openpyxl.utils import get_column_letter

APP_DIR = os.path.dirname(os.path.abspath(__file__))
WEB_DIR = os.path.join(APP_DIR, "web")
BACKUP_DIR = os.path.join(APP_DIR, "backups")
LOG_DIR = os.path.join(APP_DIR, "logs")
DATA_DIR = os.path.join(APP_DIR, "data")
WORK_DIR = os.path.join(APP_DIR, "台账")
CONFIG_PATH = os.path.join(APP_DIR, "config.json")
HISTORY_PATH = os.path.join(LOG_DIR, "history.jsonl")
POOL_PATH = os.path.join(DATA_DIR, "pool.json")
RUNTIME_PATH = os.path.join(DATA_DIR, "runtime.json")

# 首次运行想自动挂上的台账（留空则启动后在页面上选择）
DEFAULT_WB = ""
PORT = 8770

FIELDS = [
    {"key": "seq",       "col": "A", "head": "序号",             "required": False, "auto": True},
    {"key": "type",      "col": "B", "head": "类型",             "required": True,  "options": ["视频", "贴图", "推文"]},
    {"key": "title",     "col": "C", "head": "推文名称",         "required": True},
    {"key": "url",       "col": "D", "head": "推文链接",         "required": True},
    {"key": "theme",     "col": "E", "head": "推文主题",         "required": True,
     "options": ["红日、翱翔、青凝", "学院推文", "一站式学生社区", "学生组织建设"]},
    {"key": "date",      "col": "F", "head": "推文发布时间",     "required": True},
    {"key": "writer",    "col": "G", "head": "文案撰写",         "required": False},
    {"key": "layout",    "col": "H", "head": "运营排版",         "required": True},
    {"key": "account",   "col": "I", "head": "运营公众号",       "required": True},
    {"key": "art",       "col": "J", "head": "美工封面",         "required": False},
    {"key": "secretary", "col": "K", "head": "秘书处整合、归档",  "required": False},
    {"key": "photo",     "col": "L", "head": "摄影",             "required": False},
    {"key": "review",    "col": "M", "head": "审核",             "required": False},
]
FIELD_BY_KEY = {f["key"]: f for f in FIELDS}
COL_INDEX = {f["key"]: ord(f["col"]) - 64 for f in FIELDS}
LAST_COL = 13

TYPE_DEFAULT = ["视频", "贴图", "推文"]
THEME_DEFAULT = ["红日、翱翔、青凝", "学院推文", "一站式学生社区", "学生组织建设"]

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36")

LINK_RE = re.compile(r"(?:https?://)?mp\.weixin\.qq\.com/[^\s\"'<>，。；、）)】\]]+")


# ---------------------------------------------------------------- 基础工具

def ensure_dirs():
    for d in (BACKUP_DIR, LOG_DIR, DATA_DIR, WORK_DIR):
        os.makedirs(d, exist_ok=True)


def load_json(path, default):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return default


def save_json(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=1)
    os.replace(tmp, path)


def load_config():
    cfg = {"workbook": DEFAULT_WB, "date_format": "cn", "default_mode": "shift"}
    cfg.update(load_json(CONFIG_PATH, {}))
    return cfg


def save_config(cfg):
    save_json(CONFIG_PATH, cfg)


# ---------------------------------------------------------------- 抓取 / 解析

def normalize_url(url):
    url = (url or "").strip().strip('"\'')
    if not url:
        return ""
    if not re.match(r"^https?://", url, re.I):
        url = "https://" + url
    return url


def link_key(url):
    """同一篇文章的不同链接形式归一到同一个 key，用来去重"""
    u = urlparse(normalize_url(url))
    if u.path.startswith("/s/") and u.path.strip("/") != "s":
        return u.path.rstrip("/")
    return (u.path or "/") + ("?" + u.query if u.query else "")


def extract_links(text):
    text = text or ""
    found, seen, out = LINK_RE.findall(text), set(), []
    for raw in found:
        url = normalize_url(raw)
        url = url.rstrip(".,;:)]}\"'")
        k = link_key(url)
        if not k or k in seen:
            continue
        seen.add(k)
        out.append(url)
    return out


def fetch_html(url, timeout=25):
    req = urllib.request.Request(url, headers={
        "User-Agent": UA,
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language": "zh-CN,zh;q=0.9,en;q=0.6",
        "Accept-Encoding": "identity",
        "Referer": "https://mp.weixin.qq.com/",
        "Connection": "close",
    })
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
    for enc in ("utf-8", "gb18030", "latin-1"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def extract_content_div(page):
    """用 div 深度扫描精确切出 <div id="js_content"> 正文块"""
    m = re.search(r'<div[^>]*id="js_content"', page)
    if not m:
        return ""
    start = m.end()
    depth = 1
    i = start
    n = len(page)
    while i < n and depth > 0:
        nxt_open = page.find("<div", i)
        nxt_close = page.find("</div", i)
        if nxt_close == -1:
            break
        if nxt_open != -1 and nxt_open < nxt_close:
            depth += 1
            i = nxt_open + 4
        else:
            depth -= 1
            if depth == 0:
                return page[start:nxt_close]
            i = nxt_close + 5
    return page[start:min(n, start + 200000)]


def strip_tags(s):
    s = re.sub(r"(?is)<(script|style)[^>]*>.*?</\1>", " ", s)
    s = re.sub(r"(?i)<br\s*/?>", "\n", s)
    s = re.sub(r"(?i)</(p|div|section|li|h\d)>", "\n", s)
    s = re.sub(r"(?s)<[^>]+>", " ", s)
    s = htmlmod.unescape(s)
    s = s.replace("\u200b", "").replace("\xa0", " ")
    s = re.sub(r"[ \t]+", " ", s)
    s = re.sub(r"\n{2,}", "\n", s)
    return s.strip()


def first_match(patterns, text, group=1):
    for p in patterns:
        m = re.search(p, text, re.S)
        if m:
            v = m.group(group)
            v = htmlmod.unescape(v).strip()
            if v:
                return v
    return ""


def parse_article(page):
    out = {
        "title": "", "account": "", "author": "", "date": "",
        "sigs": {}, "text": "", "img_count": 0, "has_video": False,
        "warnings": [], "type_guess": "", "theme_guess": "",
    }
    if ("环境异常" in page and "js_content" not in page) or "该内容已被发布者删除" in page \
            or "此内容因违规无法查看" in page:
        out["warnings"].append("页面返回的不是正文（可能是验证页/已删除），已转为手动补全模式。")

    out["title"] = first_match([
        r"var\s+msg_title\s*=\s*'([^']*)'",
        r'var\s+msg_title\s*=\s*"((?:[^"\\]|\\.)*)"',
        r'<meta\s+property="og:title"\s+content="([^"]*)"',
        r'<meta\s+name="twitter:title"\s+content="([^"]*)"',
        r'<h1[^>]*id="activity-name"[^>]*>(.*?)</h1>',
    ], page)
    if not out["title"]:
        out["title"] = strip_tags(first_match([r"<title>(.*?)</title>"], page))

    out["account"] = first_match([
        r'var\s+nickname\s*=\s*(?:htmlDecode\()?"((?:[^"\\]|\\.)*)"',
        r'var\s+nickname\s*=\s*"((?:[^"\\]|\\.)*)"',
        r'<a[^>]*id="js_name"[^>]*>(.*?)</a>',
        r'<meta\s+property="og:article:author"\s+content="([^"]*)"',
    ], page)
    out["account"] = re.sub(r'\s*\|\|.*$', "", out["account"]).strip()

    out["author"] = first_match([
        r'var\s+author\s*=\s*"((?:[^"\\]|\\.)*)"',
        r'<meta\s+name="author"\s+content="([^"]*)"',
        r'id="js_author_name"[^>]*>(.*?)</',
    ], page)

    ts = first_match([r'var\s+ct\s*=\s*"(\d{10})"', r'"ct"\s*:\s*"(\d{10})"'], page)
    dt = None
    if ts:
        try:
            dt = datetime.datetime.fromtimestamp(int(ts))
        except Exception:
            dt = None
    if dt is None:
        s = first_match([
            r'var\s+create_time\s*=\s*"([^"]+)"',
            r'"publish_time"\s*:\s*"([^"]+)"',
            r'id="publish_time"[^>]*>(.*?)</em>',
            r'var\s+oriCreateTime\s*=\s*"([^"]+)"',
        ], page)
        m = re.search(r"(\d{4})[-/年.](\d{1,2})[-/月.](\d{1,2})", s or "")
        if m:
            try:
                dt = datetime.datetime(int(m.group(1)), int(m.group(2)), int(m.group(3)))
            except Exception:
                dt = None
    if dt:
        out["date"] = dt.strftime("%Y-%m-%d")
        out["date_cn"] = "%d年%d月%d日" % (dt.year, dt.month, dt.day)
        out["year_month"] = "%d年%d月" % (dt.year, dt.month)
    else:
        out["warnings"].append("没能从页面读到发布时间，请手动填写（格式如 2026年11月8日）。")

    body = extract_content_div(page)
    if not body and not out["title"] and not out["account"]:
        out["warnings"].append(
            "微信这次返回的是验证页（也可能文章已删除或链接过期），正文一个字段都没读到。"
            "可以直接手填，或先在浏览器里打开这篇推文再回来填。")
    out["img_count"] = len(re.findall(r"<img\b", body, re.I))
    out["has_video"] = bool(re.search(r'<mpvideo|wxv_|data-mpvid|class="video_iframe|<video\b', body, re.I))

    text = strip_tags(body)
    out["text"] = text[:6000]

    sigs = extract_signatures(text)
    if not sigs:
        sigs = extract_signatures(strip_tags(page))
    out["sigs"] = sigs

    if not out["title"]:
        out["warnings"].append("没能读到推文标题，请手动填写。")
    if not out["account"]:
        out["warnings"].append("没能读到公众号名称，请手动填写。")

    out["type_guess"] = guess_type(out)
    out["theme_guess"] = guess_theme(out)
    return out


SIG_LABELS = [
    ("图文来源", ["writer", "photo"]), ("图文素材", ["writer", "photo"]),
    ("文字来源", ["writer"]), ("图片来源", ["photo"]), ("素材来源", ["writer", "photo"]),
    ("图文", ["writer", "photo"]), ("供稿", ["writer"]), ("文案", ["writer"]),
    ("撰文", ["writer"]), ("文字", ["writer"]), ("通讯员", ["writer"]),
    ("记者", ["writer"]), ("作者", ["writer"]), ("文", ["writer"]),
    ("微信编辑", ["layout"]), ("排版", ["layout"]), ("编辑", ["layout"]),
    ("整合", ["layout"]), ("制作", ["layout"]),
    ("摄影", ["photo"]), ("摄像", ["photo"]), ("拍摄", ["photo"]),
    ("图片", ["photo"]), ("照片", ["photo"]), ("图", ["photo"]),
    ("责任编辑", ["review"]), ("责编", ["review"]), ("终审", ["review"]),
    ("初审", ["review"]), ("复审", ["review"]), ("校对", ["review"]),
    ("审核", ["review"]), ("指导老师", ["review"]), ("指导", ["review"]),
    ("封面", ["art"]), ("美编", ["art"]), ("设计", ["art"]), ("视觉", ["art"]), ("美术", ["art"]),
    ("归档", ["secretary"]), ("整理", ["secretary"]), ("秘书处", ["secretary"]),
]
SIG_FIELDS = dict(SIG_LABELS)
SIG_LABEL_RE = re.compile(
    "|".join(sorted((re.escape(k) for k, _ in SIG_LABELS), key=len, reverse=True)))
SIG_SEP_RE = re.compile(r"^[\s\u3000]*[：:|｜/]")


def extract_signatures(text):
    """从正文里抓 '编辑 | 尹雨婷' 这类署名行；同一行有多个署名会全部拆开"""
    found = {}
    if not text:
        return found
    for raw in text.split("\n"):
        line = raw.strip().strip("\u3000")
        if not line or len(line) > 120:
            continue
        hits = list(SIG_LABEL_RE.finditer(line))
        if not hits:
            continue
        for i, m in enumerate(hits):
            if not SIG_SEP_RE.match(line[m.end():m.end() + 3] or ""):
                continue
            end = hits[i + 1].start() if i + 1 < len(hits) else len(line)
            chunk = line[m.end():end].strip()
            chunk = re.sub(r"^[\s\u3000：:|｜/、]+", "", chunk)
            chunk = re.sub(r"[\s\u3000：:|｜/、,，。;；]+$", "", chunk)
            chunk = re.split(r"\s{3,}", chunk)[0].strip()
            if not chunk or len(chunk) > 30 or "http" in chunk.lower():
                continue
            for field in SIG_FIELDS[m.group(0)]:
                if field not in found:
                    found[field] = chunk
    return found


THEME_RULES = [
    ("一站式学生社区", ["一站式", "学生社区", "宿舍", "公寓", "寝室", "社区"]),
    ("学生组织建设", ["学生会", "研究生会", "团委", "换届", "招新", "纳新", "骨干",
                  "部门", "例会", "表彰", "组织建设", "青马", "培训"]),
    ("红日、翱翔、青凝", ["翱翔", "青凝", "红日", "粉体杯", "挑战杯", "科创", "大创", "创新创业",
                        "答辩", "学风", "社会实践", "三下乡", "竞赛"]),
]


def guess_theme(info):
    title = info.get("title", "") or ""
    body = (info.get("text", "") or "")[:3000]
    for blob in (title, title + " " + body):
        for theme, kws in THEME_RULES:
            for kw in kws:
                if kw in blob:
                    return theme
    return "学院推文"


def guess_type(info):
    if info.get("has_video"):
        return "视频"
    txt = info.get("text", "") or ""
    if len(txt) < 220 and info.get("img_count", 0) >= 4:
        return "贴图"
    return "推文"


# ---------------------------------------------------------------- Excel 台账

def sheet_data_rows(ws):
    rows = []
    for r in range(3, ws.max_row + 1):
        c = ws.cell(row=r, column=3).value
        d = ws.cell(row=r, column=4).value
        if (c not in (None, "")) or (d not in (None, "")):
            rows.append(r)
    return rows


def row_occupied(ws, r):
    for c in range(1, LAST_COL + 1):
        v = ws.cell(row=r, column=c).value
        if v not in (None, "") and str(v).strip() != "":
            return True
    return False


def row_snapshot(ws, r):
    snap = {}
    for f in FIELDS:
        v = ws.cell(row=r, column=COL_INDEX[f["key"]]).value
        snap[f["key"]] = "" if v is None else str(v).strip()
    return snap


def first_empty_row(ws, start):
    r = max(start, 3)
    while row_occupied(ws, r):
        r += 1
    return r


DEFAULT_FONT = Font(name="宋体", size=11)
DEFAULT_ALIGN = Alignment(horizontal="center", vertical="center", wrap_text=True)
DEFAULT_BORDER = Border()


def write_cells(ws, r, values, ref_row=None):
    for f in FIELDS:
        cell = ws.cell(row=r, column=COL_INDEX[f["key"]])
        if ref_row:
            ref = ws.cell(row=ref_row, column=COL_INDEX[f["key"]])
            try:
                cell._style = copy(ref._style)
            except Exception:
                pass
        else:
            cell.font = copy(DEFAULT_FONT)
            cell.alignment = copy(DEFAULT_ALIGN)
            cell.border = copy(DEFAULT_BORDER)
        v = values.get(f["key"], "")
        cell.value = v if v != "" else None
        if f["key"] == "seq" and v != "":
            try:
                cell.value = int(v)
            except Exception:
                cell.value = v


def shift_down(ws, at_row, count=1, blank_ref=None):
    maxr = ws.max_row
    for r in range(maxr, at_row - 1, -1):
        for c in range(1, LAST_COL + 1):
            s = ws.cell(row=r, column=c)
            d = ws.cell(row=r + count, column=c)
            d.value = s.value
            try:
                d._style = copy(s._style)
            except Exception:
                pass
    for r in range(at_row, at_row + count):
        for c in range(1, LAST_COL + 1):
            cell = ws.cell(row=r, column=c)
            cell.value = None
            if blank_ref:
                try:
                    cell._style = copy(ws.cell(row=blank_ref, column=c)._style)
                except Exception:
                    pass
            else:
                cell.font = copy(DEFAULT_FONT)
                cell.alignment = copy(DEFAULT_ALIGN)
                cell.border = copy(DEFAULT_BORDER)


def _sheet_sort_key(name):
    m = re.match(r"(d{4})年(d{1,2})月", name)
    return (int(m.group(1)), int(m.group(2))) if m else (9999, 99)


def find_or_make_sheet(wb, year_month):
    if year_month in wb.sheetnames:
        return wb[year_month]
    template, best = None, -1
    for name in wb.sheetnames:
        m = re.match(r"(\d{4})年(\d{1,2})月", name)
        if m:
            score = int(m.group(1)) * 100 + int(m.group(2))
            if score > best:
                best, template = score, name
    ws_new = wb.create_sheet(year_month)
    if not template:
        return ws_new
    src = wb[template]
    for c in range(1, LAST_COL + 1):
        s = src.cell(row=1, column=c)
        d = ws_new.cell(row=1, column=c)
        d.value = s.value
        try:
            d._style = copy(s._style)
        except Exception:
            pass
    try:
        ws_new.merge_cells(start_row=1, start_column=1, end_row=1, end_column=LAST_COL)
    except Exception:
        pass
    for c in range(1, LAST_COL + 1):
        s = src.cell(row=2, column=c)
        d = ws_new.cell(row=2, column=c)
        d.value = s.value
        try:
            d._style = copy(s._style)
        except Exception:
            pass
    ws_new.row_dimensions[1].height = src.row_dimensions[1].height or 39
    ws_new.row_dimensions[2].height = src.row_dimensions[2].height or 30
    for letter, dim in src.column_dimensions.items():
        if dim.width:
            ws_new.column_dimensions[letter].width = dim.width
    # 月份表按时间排序，新表插到该在的位置
    try:
        wb._sheets.sort(key=lambda s: _sheet_sort_key(s.title))
    except Exception:
        pass
    return ws_new


def collect_options(wb):
    types, themes, accounts, people = [], [], [], []
    for name in wb.sheetnames:
        ws = wb[name]
        for r in range(3, ws.max_row + 1):
            for f in FIELDS:
                v = ws.cell(row=r, column=COL_INDEX[f["key"]]).value
                if v in (None, ""):
                    continue
                v = str(v).strip()
                if not v or v == "/" or re.match(r"^x+$", v, re.I):
                    continue
                if f["key"] == "type" and v not in types:
                    types.append(v)
                elif f["key"] == "theme" and v not in themes:
                    themes.append(v)
                elif f["key"] == "account" and v not in accounts:
                    accounts.append(v)
                elif f["key"] in ("writer", "layout", "photo", "art", "review", "secretary") and v not in people:
                    people.append(v)
    for v in TYPE_DEFAULT:
        if v not in types:
            types.append(v)
    for v in THEME_DEFAULT:
        if v not in themes:
            themes.append(v)
    return types, themes, accounts, people


def ledger_urls(path):
    if not path or not os.path.exists(path):
        return set()
    try:
        wb = load_workbook(path, read_only=True, data_only=True)
    except Exception:
        return set()
    urls = set()
    for name in wb.sheetnames:
        ws = wb[name]
        for row in ws.iter_rows(min_row=3, max_col=4, values_only=True):
            v = row[3] if len(row) > 3 else None
            if isinstance(v, str) and "mp.weixin.qq.com" in v:
                urls.add(link_key(v))
    try:
        wb.close()
    except Exception:
        pass
    return urls


def open_wb(path):
    if not path or not os.path.exists(path):
        raise FileNotFoundError("台账文件不存在：%s" % path)
    return load_workbook(path)


def is_writable_file(path):
    try:
        with open(path, "r+b"):
            return True
    except Exception:
        return False


def import_working_copy(src):
    ensure_dirs()
    dst = os.path.join(WORK_DIR, os.path.basename(src))
    if os.path.exists(dst):
        ts = time.strftime("%Y%m%d-%H%M%S")
        shutil.move(dst, os.path.join(BACKUP_DIR, "%s_旧副本_%s" % (ts, os.path.basename(src))))
    shutil.copy2(src, dst)
    os.chmod(dst, stat.S_IWRITE | stat.S_IREAD)
    return dst


def prepare_target(path):
    if not path or not os.path.exists(path):
        raise FileNotFoundError("台账文件不存在：%s" % path)
    if is_writable_file(path):
        return path, ""
    dst = import_working_copy(path)
    return dst, "原文件是只读的（微信接收的文件默认如此），已生成工作副本：%s" % dst


def backup_file(path):
    ensure_dirs()
    ts = time.strftime("%Y%m%d-%H%M%S")
    dst = os.path.join(BACKUP_DIR, "%s_%s" % (ts, os.path.basename(path)))
    i = 1
    while os.path.exists(dst):
        dst = os.path.join(BACKUP_DIR, "%s_%d_%s" % (ts, i, os.path.basename(path)))
        i += 1
    shutil.copyfile(path, dst)
    try:
        os.chmod(dst, stat.S_IWRITE | stat.S_IREAD)
    except Exception:
        pass
    files = sorted(f for f in os.listdir(BACKUP_DIR) if f.lower().endswith((".xlsx", ".xlsm")))
    while len(files) > 40:
        try:
            os.remove(os.path.join(BACKUP_DIR, files.pop(0)))
        except Exception:
            break
    return dst


def log_history(entry):
    ensure_dirs()
    entry["ts"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(HISTORY_PATH, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(entry, ensure_ascii=False) + "\n")


def save_wb(wb, path):
    tmp = path + ".saving"
    wb.save(tmp)
    if os.path.exists(path):
        try:
            os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
        except Exception:
            pass
    os.replace(tmp, path)


def year_month_of(date_str):
    if not date_str:
        return ""
    m = re.search(r"(\d{4})\D+(\d{1,2})", str(date_str))
    if not m:
        return ""
    return "%d年%d月" % (int(m.group(1)), int(m.group(2)))


def _write_into(wb, values, mode):
    ym = year_month_of(values.get("date", ""))
    if not ym:
        raise ValueError("缺少发布时间，定位不到哪张月表")
    created = ym not in wb.sheetnames
    ws = find_or_make_sheet(wb, ym)
    recs = sheet_data_rows(ws)
    ref_row = recs[-1] if recs else None
    target = (ref_row + 1) if ref_row else 3
    if row_occupied(ws, target):
        if mode == "next_empty":
            target = first_empty_row(ws, target)
        elif mode != "overwrite":
            shift_down(ws, target, 1, blank_ref=ref_row)
    seq = len(recs) + 1
    v = dict(values)
    v["seq"] = str(seq)
    write_cells(ws, target, v, ref_row=ref_row)
    return {"sheet": ym, "row": target, "seq": seq, "sheet_created": created}


def plan_write(path, date_str, mode, values=None):
    wb = open_wb(path)
    ym = year_month_of(date_str) or wb.sheetnames[-1]
    existed = ym in wb.sheetnames
    ws = find_or_make_sheet(wb, ym)
    recs = sheet_data_rows(ws)
    ref_row = recs[-1] if recs else None
    target = (ref_row + 1) if ref_row else 3
    if row_occupied(ws, target) and mode == "next_empty":
        target = first_empty_row(ws, target)
    occupied = row_occupied(ws, target)
    plan = {"sheet": ym, "sheet_created": not existed, "row": target,
            "seq": len(recs) + 1, "occupied": occupied, "records": len(recs),
            "sheets": wb.sheetnames}
    if occupied:
        plan["occupied_cells"] = [
            {"cell": "%s%d" % (get_column_letter(c), target),
             "value": str(ws.cell(row=target, column=c).value)}
            for c in range(1, LAST_COL + 1)
            if ws.cell(row=target, column=c).value not in (None, "")
        ]
    return plan


def do_write(path, values, mode):
    result = plan_write(path, values.get("date", ""), mode)
    backup = backup_file(path)
    wb = open_wb(path)
    res = _write_into(wb, values, mode)
    save_wb(wb, path)
    res["backup"] = os.path.basename(backup)
    log_history({"action": "write", "sheet": res["sheet"], "row": res["row"],
                 "title": values.get("title", ""), "url": values.get("url", "")})
    res.update({k: result[k] for k in ("occupied", "records", "sheets") if k in result})
    return res


def do_write_many(path, items, mode="shift"):
    """一次打开一次保存：批量写入，每条写完才知道下一条该落哪行"""
    backup = backup_file(path)
    wb = open_wb(path)
    results = []
    for it in items:
        try:
            res = _write_into(wb, it["values"], mode)
            res["id"] = it.get("id")
            res["ok"] = True
            results.append(res)
        except Exception as e:
            results.append({"id": it.get("id"), "ok": False, "error": str(e)})
    save_wb(wb, path)
    log_history({"action": "write_many", "count": len(results),
                 "ok": sum(1 for r in results if r["ok"])})
    return {"ok": True, "results": results, "sheets": wb.sheetnames,
            "backup": os.path.basename(backup)}


# ---------------------------------------------------------------- 链接池

POOL_LOCK = threading.Lock()


def load_pool():
    data = load_json(POOL_PATH, {"items": []})
    if not isinstance(data, dict) or "items" not in data:
        data = {"items": []}
    return data


def save_pool(pool):
    save_json(POOL_PATH, pool)


def pool_find(pool, url):
    k = link_key(url)
    for it in pool["items"]:
        if link_key(it.get("url", "")) == k:
            return it
    return None


def make_item(parsed, url, source):
    sigs = parsed.get("sigs") or {}
    return {
        "id": uuid.uuid4().hex[:10],
        "url": url,
        "title": parsed.get("title", ""),
        "account": parsed.get("account", ""),
        "date": parsed.get("date_cn") or parsed.get("date") or "",
        "type": parsed.get("type_guess", ""),
        "theme": parsed.get("theme_guess", ""),
        "writer": sigs.get("writer", ""),
        "layout": sigs.get("layout", ""),
        "photo": sigs.get("photo", ""),
        "review": sigs.get("review", ""),
        "art": sigs.get("art", ""),
        "secretary": sigs.get("secretary", ""),
        "sigs": sigs,
        "warnings": parsed.get("warnings", []),
        "status": "parsed" if parsed.get("title") else "partial",
        "source": source,
        "created": time.strftime("%Y-%m-%d %H:%M:%S"),
        "written": None,
    }


def pool_add(pool, item):
    existing = pool_find(pool, item["url"])
    if existing:
        keep_written = existing.get("written")
        existing.update(item)
        existing["id"] = existing.get("id") or item["id"]
        existing["written"] = keep_written
        return existing, False
    pool["items"].insert(0, item)
    return item, True


JOB_STATE = {"running": False, "total": 0, "done": 0, "added": 0, "failed": 0,
             "source": "", "started": "", "finished": ""}
JOB_LOCK = threading.Lock()


def job_reset(total, source):
    with JOB_LOCK:
        JOB_STATE.update({"running": True, "total": total, "done": 0, "added": 0,
                          "failed": 0, "source": source,
                          "started": time.strftime("%H:%M:%S"), "finished": ""})


def job_update(**kw):
    with JOB_LOCK:
        JOB_STATE.update(kw)


def job_snapshot():
    with JOB_LOCK:
        return dict(JOB_STATE)


def fetch_worker(urls, source, force=False):
    job_reset(len(urls), source)
    try:
        for url in urls:
            try:
                with POOL_LOCK:
                    pool = load_pool()
                    exist = pool_find(pool, url)
                if exist and exist.get("status") == "parsed" and not force:
                    job_update(done=JOB_STATE["done"] + 1)
                    continue
                page = fetch_html(url)
                parsed = parse_article(page)
                item = make_item(parsed, url, source)
                with POOL_LOCK:
                    pool = load_pool()
                    pool_add(pool, item)
                    save_pool(pool)
                job_update(done=JOB_STATE["done"] + 1,
                           added=JOB_STATE["added"] + 1)
            except Exception as e:
                parsed = {"warnings": ["抓取失败：%s" % e]}
                item = make_item(parsed, url, source)
                item["status"] = "error"
                with POOL_LOCK:
                    pool = load_pool()
                    pool_add(pool, item)
                    save_pool(pool)
                job_update(done=JOB_STATE["done"] + 1,
                           failed=JOB_STATE["failed"] + 1)
            time.sleep(0.35)
    finally:
        job_update(running=False, finished=time.strftime("%H:%M:%S"))


def item_values(item, fill_slash=False):
    vals = {f["key"]: str(item.get(f["key"], "") or "").strip() for f in FIELDS}
    vals["url"] = item.get("url", "")
    if fill_slash:
        for f in FIELDS:
            if f["required"] and f["key"] != "seq":
                continue
            if not vals.get(f["key"]):
                vals[f["key"]] = "/"
    return vals


def item_missing(item):
    return [f["head"] for f in FIELDS
            if f["required"] and f["key"] != "seq" and not str(item.get(f["key"], "") or "").strip()]


# ---------------------------------------------------------------- 悬浮按钮

RUNTIME_LOCK = threading.Lock()


def runtime_path():
    return RUNTIME_PATH


def read_runtime():
    return load_json(RUNTIME_PATH, {})


def write_runtime(data):
    save_json(RUNTIME_PATH, data)


def floater_alive(pid):
    if not pid:
        return False
    try:
        out = subprocess.run(["tasklist", "/FI", "PID eq %d" % int(pid)],
                             capture_output=True, timeout=8)
        return str(pid) in out.stdout.decode("gbk", "ignore")
    except Exception:
        return False


def start_floater(port):
    rt = read_runtime()
    pid = rt.get("floater_pid")
    if pid and floater_alive(pid):
        return {"ok": True, "pid": pid, "already": True}
    pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    if not os.path.exists(pyw):
        pyw = sys.executable
    script = os.path.join(APP_DIR, "floater.py")
    if not os.path.exists(script):
        return {"ok": False, "error": "找不到 floater.py"}
    flags = 0
    if os.name == "nt":
        flags = 0x00000008 | 0x00000200  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen([pyw, script, "--port", str(port)], cwd=APP_DIR,
                            creationflags=flags, close_fds=True,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rt["floater_pid"] = proc.pid
    write_runtime(rt)
    return {"ok": True, "pid": proc.pid}


def stop_floater():
    rt = read_runtime()
    pid = rt.get("floater_pid")
    if not pid:
        return {"ok": True, "already": True}
    try:
        subprocess.run(["taskkill", "/F", "/PID", str(int(pid))],
                       capture_output=True, timeout=10)
    except Exception:
        pass
    rt.pop("floater_pid", None)
    write_runtime(rt)
    return {"ok": True}


# ---------------------------------------------------------------- HTTP

class Handler(BaseHTTPRequestHandler):
    server_version = "WeChatLedger/1.1"

    def log_message(self, fmt, *args):
        try:
            sys.stdout.write("[%s] %s\n" % (time.strftime("%H:%M:%S"), fmt % args))
            sys.stdout.flush()
        except Exception:
            pass

    # ---------- helpers
    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False)
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _json(self, obj, code=200):
        self._send(code, obj)

    def _read_json(self):
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    def _read_bytes(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def _static(self, path):
        rel = unquote(path.lstrip("/").split("?")[0] or "index.html")
        if rel == "":
            rel = "index.html"
        full = os.path.normpath(os.path.join(WEB_DIR, rel))
        if not full.startswith(os.path.normpath(WEB_DIR)) or not os.path.isfile(full):
            self._send(404, "not found", "text/plain; charset=utf-8")
            return
        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype == "application/javascript":
            ctype += "; charset=utf-8"
        with open(full, "rb") as fh:
            data = fh.read()
        self._send(200, data, ctype)

    # ---------- GET
    def do_GET(self):
        p = urlparse(self.path).path
        try:
            if p in ("/", "") or p.endswith((".html", ".css", ".js", ".svg", ".ico", ".png", ".jpg")):
                return self._static(p if p != "/" else "/index.html")
            if p == "/api/state":
                return self.api_state()
            if p == "/api/rows":
                return self.api_rows()
            if p == "/api/pool":
                return self.api_pool()
            if p == "/api/ping":
                return self._json({"ok": True, "pending": self.pool_pending(),
                                   "job": job_snapshot()})
            if p == "/api/floating/status":
                rt = read_runtime()
                return self._json({"running": floater_alive(rt.get("floater_pid")),
                                   "pid": rt.get("floater_pid")})
            self._send(404, {"error": "unknown route"})
        except Exception:
            self._send(500, {"error": traceback.format_exc(limit=4)})

    # ---------- POST
    def do_POST(self):
        p = urlparse(self.path).path
        try:
            if p == "/api/parse":
                return self.api_parse()
            if p == "/api/preview":
                return self.api_preview()
            if p == "/api/append":
                return self.api_append()
            if p == "/api/config":
                return self.api_config()
            if p == "/api/pick-file":
                return self.api_pick_file()
            if p == "/api/undo":
                return self.api_undo()
            if p == "/api/open-file":
                return self.api_open_file()
            if p == "/api/urls/extract":
                return self.api_urls_extract()
            if p == "/api/urls/upload":
                return self.api_urls_upload()
            if p == "/api/pool/fetch":
                return self.api_pool_fetch()
            if p == "/api/pool/remove":
                return self.api_pool_remove()
            if p == "/api/pool/clear":
                return self.api_pool_clear()
            if p == "/api/pool/update":
                return self.api_pool_update()
            if p == "/api/pool/write":
                return self.api_pool_write()
            if p == "/api/capture":
                return self.api_capture()
            if p == "/api/floating/hello":
                body = self._read_json()
                rt = read_runtime()
                rt["floater_pid"] = int(body.get("pid") or 0)
                write_runtime(rt)
                return self._json({"ok": True, "port": self.server.server_address[1]})
            if p == "/api/floating/start":
                return self._json(start_floater(self.server.server_address[1]))
            if p == "/api/floating/stop":
                return self._json(stop_floater())
            self._send(404, {"error": "unknown route"})
        except Exception:
            self._send(500, {"error": traceback.format_exc(limit=4)})

    # ---------- pool helpers
    def pool_pending(self):
        pool = load_pool()
        return sum(1 for it in pool["items"] if not it.get("written"))

    def api_state(self):
        cfg = load_config()
        note = ""
        if os.path.exists(cfg["workbook"]) and not is_writable_file(cfg["workbook"]):
            try:
                newp, note = prepare_target(cfg["workbook"])
                cfg["workbook"] = newp
                save_config(cfg)
            except Exception as e:
                note = "工作副本创建失败：%s" % e
        info = {"workbook": cfg["workbook"], "exists": bool(cfg["workbook"]) and os.path.exists(cfg["workbook"]),
                "date_format": cfg.get("date_format", "cn"),
                "default_mode": cfg.get("default_mode", "shift"),
                "note": note, "work_dir": WORK_DIR,
                "fields": FIELDS, "sheets": [], "records": {}, "options": {}}
        if info["exists"]:
            wb = open_wb(cfg["workbook"])
            info["sheets"] = wb.sheetnames
            for name in wb.sheetnames:
                info["records"][name] = len(sheet_data_rows(wb[name]))
            types, themes, accounts, people = collect_options(wb)
            info["options"] = {"types": types, "themes": themes,
                               "accounts": accounts, "people": people}
        info["floating"] = floater_alive(read_runtime().get("floater_pid"))
        info["poolPending"] = self.pool_pending()
        return self._json(info)

    def api_rows(self):
        cfg = load_config()
        wb = open_wb(cfg["workbook"])
        out = {}
        for name in wb.sheetnames:
            ws = wb[name]
            rows = []
            for r in sheet_data_rows(ws):
                snap = row_snapshot(ws, r)
                snap["row"] = r
                rows.append(snap)
            out[name] = rows
        return self._json({"sheets": wb.sheetnames, "rows": out})

    def api_parse(self):
        body = self._read_json()
        url = normalize_url(body.get("url", ""))
        if not url:
            return self._json({"ok": False, "error": "请先粘贴公众号文章链接"}, 400)
        if "mp.weixin.qq.com" not in url:
            return self._json({"ok": False,
                               "error": "这看起来不是微信公众号文章链接（应为 https://mp.weixin.qq.com/s/...）"}, 400)
        try:
            page = fetch_html(url)
        except urllib.error.HTTPError as e:
            return self._json({"ok": False, "error": "抓取失败：HTTP %s" % e.code}, 502)
        except Exception as e:
            return self._json({"ok": False, "error": "抓取失败：%s" % e}, 502)
        info = parse_article(page)
        info["url"] = url
        info["ok"] = True
        return self._json(info)

    def api_preview(self):
        body = self._read_json()
        cfg = load_config()
        try:
            plan = plan_write(cfg["workbook"], body.get("date", ""),
                              body.get("mode") or cfg.get("default_mode", "shift"))
        except Exception as e:
            return self._json({"ok": False, "error": str(e)}, 400)
        plan["ok"] = True
        return self._json(plan)

    def api_append(self):
        body = self._read_json()
        cfg = load_config()
        values = body.get("values") or {}
        missing = [f["head"] for f in FIELDS
                   if f["required"] and f["key"] != "seq" and not str(values.get(f["key"], "")).strip()]
        if missing:
            return self._json({"ok": False, "error": "还有必填项没填：" + "、".join(missing),
                               "missing": missing}, 400)
        try:
            res = do_write(cfg["workbook"], values, body.get("mode") or cfg.get("default_mode", "shift"))
            res["ok"] = True
            return self._json(res)
        except PermissionError:
            return self._json({"ok": False,
                               "error": "台账正被 Excel 占用，先关掉表格再写入（已备份）。"}, 423)
        except Exception as e:
            return self._json({"ok": False, "error": "写入失败：%s" % e}, 400)

    def api_config(self):
        body = self._read_json()
        cfg = load_config()
        note = ""
        if body.get("workbook"):
            raw = body["workbook"].strip().strip('"')
            try:
                cfg["workbook"], note = prepare_target(raw)
            except Exception as e:
                return self._json({"ok": False, "error": str(e)}, 400)
        if body.get("date_format"):
            cfg["date_format"] = body["date_format"]
        if body.get("default_mode"):
            cfg["default_mode"] = body["default_mode"]
        save_config(cfg)
        return self._json({"ok": True, "workbook": cfg["workbook"], "note": note,
                           "exists": os.path.exists(cfg["workbook"])})

    def api_pick_file(self):
        code = (
            "import tkinter as tk\n"
            "from tkinter import filedialog\n"
            "r = tk.Tk(); r.withdraw(); r.attributes('-topmost', True)\n"
            "p = filedialog.askopenfilename(title='选择台账 Excel 文件',"
            " filetypes=[('Excel 工作簿','*.xlsx *.xlsm')])\n"
            "print(p or '')\n"
        )
        try:
            out = subprocess.run([sys.executable, "-c", code], capture_output=True, timeout=180)
            path = out.stdout.decode("utf-8", "replace").strip()
        except Exception as e:
            return self._json({"ok": False, "error": "打开文件对话框失败：%s" % e}, 400)
        if not path:
            return self._json({"ok": False, "cancelled": True})
        cfg = load_config()
        try:
            cfg["workbook"], note = prepare_target(path)
        except Exception as e:
            return self._json({"ok": False, "error": str(e)}, 400)
        save_config(cfg)
        return self._json({"ok": True, "workbook": cfg["workbook"], "note": note})

    def api_undo(self):
        cfg = load_config()
        path = cfg["workbook"]
        base = os.path.splitext(os.path.basename(path))[0]
        files = [f for f in sorted(os.listdir(BACKUP_DIR))
                 if f.lower().endswith((".xlsx", ".xlsm")) and base in f] \
            if os.path.exists(BACKUP_DIR) else []
        if not files:
            return self._json({"ok": False, "error": "没有可还原的备份"}, 400)
        latest = os.path.join(BACKUP_DIR, files[-1])
        shutil.copy2(latest, path)
        log_history({"action": "undo", "from": files[-1]})
        return self._json({"ok": True, "restored": files[-1]})

    def api_open_file(self):
        cfg = load_config()
        path = cfg["workbook"]
        if not path or not os.path.exists(path):
            return self._json({"ok": False, "error": "还没有绑定台账文件"}, 400)
        try:
            os.startfile(path)
            return self._json({"ok": True})
        except Exception as e:
            return self._json({"ok": False, "error": str(e)}, 400)

    # ---------- 批量导入 / 链接池
    def api_urls_extract(self):
        body = self._read_json()
        links = extract_links(body.get("text", ""))
        return self._json({"ok": True, "links": links, "count": len(links)})

    def api_urls_upload(self):
        name = unquote(self.headers.get("X-Filename") or "upload")
        raw = self._read_bytes()
        if not raw:
            return self._json({"ok": False, "error": "文件是空的"}, 400)
        text = ""
        low = name.lower()
        if low.endswith((".xlsx", ".xlsm")):
            try:
                wb = load_workbook(io.BytesIO(raw), read_only=True, data_only=True)
            except Exception as e:
                return self._json({"ok": False, "error": "读不了这个 Excel：%s" % e}, 400)
            chunks = []
            for ws in wb.worksheets:
                for row in ws.iter_rows(values_only=True):
                    for cell in row:
                        if isinstance(cell, str):
                            chunks.append(cell)
            try:
                wb.close()
            except Exception:
                pass
            text = "\n".join(chunks)
        elif low.endswith((".csv", ".txt", ".md", ".html", ".htm")):
            for enc in ("utf-8-sig", "utf-8", "gb18030"):
                try:
                    text = raw.decode(enc)
                    break
                except UnicodeDecodeError:
                    continue
        else:
            return self._json({"ok": False, "error": "只支持 .xlsx/.xlsm/.csv/.txt/.md/.html"}, 400)
        links = extract_links(text)
        return self._json({"ok": True, "links": links, "count": len(links), "filename": name})

    def api_pool(self):
        pool = load_pool()
        cfg = load_config()
        have = ledger_urls(cfg.get("workbook", ""))
        items = []
        for it in pool["items"]:
            x = dict(it)
            x["in_ledger"] = link_key(it.get("url", "")) in have
            x["missing"] = item_missing(it)
            items.append(x)
        return self._json({"ok": True, "items": items, "job": job_snapshot(),
                           "pending": sum(1 for it in pool["items"] if not it.get("written")),
                           "total": len(pool["items"])})

    def api_pool_fetch(self):
        body = self._read_json()
        urls = [normalize_url(u) for u in (body.get("urls") or []) if u]
        urls = [u for u in urls if "mp.weixin.qq.com" in u]
        source = body.get("source") or "paste"
        if not urls:
            return self._json({"ok": False, "error": "没有可抓取的链接"}, 400)
        if job_snapshot()["running"]:
            return self._json({"ok": False, "error": "上一批还在抓取，稍等一下"}, 409)
        th = threading.Thread(target=fetch_worker, args=(urls, source, bool(body.get("force"))),
                              daemon=True)
        th.start()
        return self._json({"ok": True, "queued": len(urls)})

    def api_pool_remove(self):
        body = self._read_json()
        ids = set(body.get("ids") or [])
        with POOL_LOCK:
            pool = load_pool()
            pool["items"] = [it for it in pool["items"] if it["id"] not in ids]
            save_pool(pool)
        return self._json({"ok": True})

    def api_pool_clear(self):
        with POOL_LOCK:
            pool = load_pool()
            pool["items"] = [] if self._read_json().get("all") else \
                [it for it in pool["items"] if it.get("written")]
            save_pool(pool)
        return self._json({"ok": True})

    def api_pool_update(self):
        body = self._read_json()
        pid = body.get("id")
        patch = body.get("patch") or {}
        with POOL_LOCK:
            pool = load_pool()
            for it in pool["items"]:
                if it["id"] == pid:
                    for k, v in patch.items():
                        if k in ("title", "account", "date", "type", "theme", "writer",
                                 "layout", "photo", "review", "art", "secretary", "url"):
                            it[k] = v
                    it["status"] = "parsed" if it.get("title") else "partial"
                    break
            save_pool(pool)
        return self._json({"ok": True})

    def api_pool_write(self):
        body = self._read_json()
        ids = body.get("ids")
        fill_slash = bool(body.get("fillSlash"))
        mode = body.get("mode") or "shift"
        cfg = load_config()
        if not cfg.get("workbook") or not os.path.exists(cfg["workbook"]):
            return self._json({"ok": False, "error": "还没有绑定台账文件"}, 400)
        with POOL_LOCK:
            pool = load_pool()
            chosen = [it for it in pool["items"]
                      if (ids is None or it["id"] in ids) and not it.get("written")]
        ready, skipped = [], []
        for it in chosen:
            miss = item_missing(it)
            if miss and not fill_slash:
                skipped.append({"id": it["id"], "title": it.get("title"), "missing": miss})
                continue
            if miss and fill_slash:
                fatal = [m for m in miss if m in ("推文发布时间", "推文链接")]
                if fatal:
                    skipped.append({"id": it["id"], "title": it.get("title"), "missing": fatal})
                    continue
            ready.append({"id": it["id"], "values": item_values(it, fill_slash)})
        if not ready:
            return self._json({"ok": False, "error": "没有可以写入的条目",
                               "skipped": skipped}, 400)
        try:
            res = do_write_many(cfg["workbook"], ready, mode)
        except PermissionError:
            return self._json({"ok": False, "error": "台账正被 Excel 占用，先关掉表格再写入"}, 423)
        except Exception as e:
            return self._json({"ok": False, "error": "写入失败：%s" % e}, 400)
        by_id = {r["id"]: r for r in res["results"]}
        with POOL_LOCK:
            pool = load_pool()
            for it in pool["items"]:
                r = by_id.get(it["id"])
                if r and r.get("ok"):
                    it["written"] = {"sheet": r["sheet"], "row": r["row"], "seq": r["seq"],
                                     "at": time.strftime("%Y-%m-%d %H:%M:%S")}
            save_pool(pool)
        res["skipped"] = skipped
        res["written"] = sum(1 for r in res["results"] if r.get("ok"))
        return self._json(res)

    def api_capture(self):
        body = self._read_json()
        urls = extract_links(body.get("url", "")) or (
            [normalize_url(body.get("url", ""))] if body.get("url") else [])
        if not urls:
            return self._json({"ok": False, "error": "没有识别到公众号链接"}, 400)
        url = urls[0]
        try:
            page = fetch_html(url)
            parsed = parse_article(page)
            item = make_item(parsed, url, body.get("source") or "floating")
            if not item["title"] and parsed.get("warnings"):
                item["status"] = "partial"
            with POOL_LOCK:
                pool = load_pool()
                saved, is_new = pool_add(pool, item)
                save_pool(pool)
            return self._json({"ok": True, "item": item, "new": is_new,
                               "pending": sum(1 for it in load_pool()["items"] if not it.get("written"))})
        except Exception as e:
            return self._json({"ok": False, "error": str(e)}, 502)


def bootstrap():
    ensure_dirs()
    cfg = load_config()
    try:
        if DEFAULT_WB and not os.path.exists(cfg["workbook"]) and os.path.exists(DEFAULT_WB):
            cfg["workbook"], _ = prepare_target(DEFAULT_WB)
            save_config(cfg)
        elif cfg["workbook"] and os.path.exists(cfg["workbook"]) and not is_writable_file(cfg["workbook"]):
            cfg["workbook"], _ = prepare_target(cfg["workbook"])
            save_config(cfg)
    except Exception as e:
        print("初始化台账失败：%s" % e)


def main():
    ensure_dirs()
    bootstrap()
    port = PORT
    httpd = None
    for _ in range(20):
        try:
            httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
            break
        except OSError:
            port += 1
    if httpd is None:
        raise SystemExit("没有可用端口")
    rt = read_runtime()
    rt["port"] = port
    rt["started"] = time.strftime("%Y-%m-%d %H:%M:%S")
    write_runtime(rt)
    url = "http://127.0.0.1:%d/" % port
    print("=" * 56)
    print("  推文台账助手：%s" % url)
    print("  台账：%s" % (load_config()["workbook"] or "（未绑定，请在页面上选择）"))
    print("  关闭此窗口即停止服务")
    print("=" * 56)
    sys.stdout.flush()
    if "--no-browser" not in sys.argv:
        try:
            import webbrowser
            webbrowser.open(url)
        except Exception:
            pass
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    main()

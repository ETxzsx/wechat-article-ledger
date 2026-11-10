// wechat_link_watcher.cpp  (v4 · 全电脑自动记录 + 内存/缓存治理)
// 打开即监控，两路来源自动记录公众号文章链接：
//   1) 浏览器历史：Chrome/Edge/Brave/360/QQ/360Chrome 的 History + Firefox 的 places.sqlite
//   2) 全局剪贴板：任何程序复制公众号链接立即记录，并抓取文章标题。
// 资源治理：全部句柄 RAII 自动释放；标题抓取带缓存（同篇不重复请求）；记录批量落盘减少 IO；
//          Ctrl+C 优雅退出（flush + 关文件 + 注销监听）。
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;
using SteadyClock = std::chrono::steady_clock;

// ---------------- SQLite 动态加载 ----------------
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef int (*p_open_v2)(const char*, sqlite3**, int, const char*);
typedef int (*p_prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
typedef int (*p_step)(sqlite3_stmt*);
typedef const unsigned char* (*p_column_text)(sqlite3_stmt*, int);
typedef int (*p_finalize)(sqlite3_stmt*);
typedef int (*p_close)(sqlite3*);

p_open_v2      g_open_v2 = nullptr;
p_prepare_v2   g_prepare = nullptr;
p_step         g_step = nullptr;
p_column_text  g_coltext = nullptr;
p_finalize     g_finalize = nullptr;
p_close        g_close = nullptr;

static bool load_sqlite() {
    HMODULE h = LoadLibraryW(L"winsqlite3.dll");
    if (!h) h = LoadLibraryW(L"sqlite3.dll");
    if (!h) { printf("[错误] 找不到 winsqlite3.dll / sqlite3.dll\n"); return false; }
    g_open_v2  = (p_open_v2)GetProcAddress(h, "sqlite3_open_v2");
    g_prepare  = (p_prepare_v2)GetProcAddress(h, "sqlite3_prepare_v2");
    g_step     = (p_step)GetProcAddress(h, "sqlite3_step");
    g_coltext  = (p_column_text)GetProcAddress(h, "sqlite3_column_text");
    g_finalize = (p_finalize)GetProcAddress(h, "sqlite3_finalize");
    g_close    = (p_close)GetProcAddress(h, "sqlite3_close");
    return g_open_v2 && g_prepare && g_step && g_coltext && g_finalize && g_close;
}

// ---------------- RAII 句柄 ----------------
struct SqliteDb {
    sqlite3* db = nullptr;
    SqliteDb() = default;
    ~SqliteDb() { close(); }
    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;
    bool open(const std::string& uri, int flags) { return g_open_v2(uri.c_str(), &db, flags, nullptr) == 0; }
    void close() { if (db && g_close) { g_close(db); db = nullptr; } }
    sqlite3* get() const { return db; }
    explicit operator bool() const { return db != nullptr; }
};

struct SqliteStmt {
    sqlite3_stmt* st = nullptr;
    SqliteStmt() = default;
    ~SqliteStmt() { if (st && g_finalize) g_finalize(st); }
    SqliteStmt(const SqliteStmt&) = delete;
    SqliteStmt& operator=(const SqliteStmt&) = delete;
    bool prepare(sqlite3* db, const char* q) { return g_prepare(db, q, -1, &st, nullptr) == 0; }
    bool row() { return g_step(st) == 100; }
    const char* text(int i) {
        const unsigned char* p = g_coltext(st, i);
        return p ? (const char*)p : "";
    }
};

struct ClipboardGuard {
    bool ok = false;
    ClipboardGuard() { ok = OpenClipboard(nullptr) != 0; }
    ~ClipboardGuard() { if (ok) CloseClipboard(); }
    explicit operator bool() const { return ok; }
};

struct GlobalLockGuard {
    HANDLE h = nullptr;
    void* p = nullptr;
    explicit GlobalLockGuard(HANDLE h_) : h(h_) { if (h) p = GlobalLock(h); }
    ~GlobalLockGuard() { if (h && p) GlobalUnlock(h); }
    void* get() const { return p; }
};

struct HttpHandle {
    HINTERNET h = nullptr;
    explicit HttpHandle(HINTERNET h_ = nullptr) : h(h_) {}
    ~HttpHandle() { if (h) WinHttpCloseHandle(h); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    HttpHandle(HttpHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    operator HINTERNET() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};

// ---------------- 工具 ----------------
static std::string w2u(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring u2w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

static std::string now_str() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

static std::string csv_escape(const std::string& s) {
    bool need = s.find_first_of(",\"\r\n") != std::string::npos;
    std::string out;
    if (need) out.push_back('"');
    for (char c : s) { if (c == '"') out += "\"\""; else out.push_back(c); }
    if (need) out.push_back('"');
    return out;
}

static std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c);
        }
    }
    return out;
}

// ---------------- 文章标识 ----------------
static bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

static std::string article_key(const std::string& raw) {
    std::string url = raw;
    size_t pos = url.find("://");
    std::string body = (pos == std::string::npos) ? url : url.substr(pos + 3);
    std::string host = body.substr(0, body.find('/'));
    if (host != "mp.weixin.qq.com") return {};
    std::string path_query = body.substr(body.find('/'));
    if (starts_with(path_query, "/s/")) {
        std::string id = path_query.substr(3);
        size_t q = id.find_first_of("?#");
        if (q != std::string::npos) id = id.substr(0, q);
        id.erase(std::remove(id.begin(), id.end(), '/'), id.end());
        if (id.size() >= 6) return "s/" + id;
        return {};
    }
    if (starts_with(path_query, "/s?")) {
        auto getq = [&](const std::string& k) -> std::string {
            size_t p = path_query.find(k + "=");
            if (p == std::string::npos) return {};
            std::string v = path_query.substr(p + k.size() + 1);
            size_t e = v.find_first_of("&#");
            if (e != std::string::npos) v = v.substr(0, e);
            return v;
        };
        std::string biz = getq("__biz");
        std::string mid = getq("mid");
        if (mid.empty()) mid = getq("sn");
        if (!biz.empty() && !mid.empty()) return "l/" + biz + "/" + mid;
        return {};
    }
    return {};
}

static std::vector<std::string> extract_weixin_links(const std::string& text) {
    std::vector<std::string> out;
    const std::string marker = "mp.weixin.qq.com";
    size_t pos = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos) {
        size_t start = pos;
        if (pos >= 8 && text.compare(pos - 8, 8, "https://") == 0) start = pos - 8;
        else if (pos >= 7 && text.compare(pos - 7, 7, "http://") == 0) start = pos - 7;
        size_t end = pos + marker.size();
        while (end < text.size()) {
            unsigned char c = (unsigned char)text[end];
            if (c >= 0x80) break;
            if (c == ' ' || c == '"' || c == '\'' || c == '<' || c == '>' ||
                c == '\r' || c == '\n' || c == '\t') break;
            ++end;
        }
        std::string url = text.substr(start, end - start);
        if (!url.empty() && !article_key(url).empty()) {
            if (std::find(out.begin(), out.end(), url) == out.end()) out.push_back(url);
        }
        pos = end;
    }
    return out;
}

// ---------------- 标题抓取（WinHTTP + 缓存） ----------------
static std::string html_unescape(std::string s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t e = s.find(';', i);
            if (e != std::string::npos && e - i <= 8) {
                std::string ent = s.substr(i + 1, e - i - 1);
                if (ent == "amp") out.push_back('&');
                else if (ent == "lt") out.push_back('<');
                else if (ent == "gt") out.push_back('>');
                else if (ent == "quot") out.push_back('"');
                else if (ent == "apos") out.push_back('\'');
                else if (ent == "nbsp") out.push_back(' ');
                else out += s.substr(i, e - i + 1);
                i = e;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

static std::string extract_title_from_html(const std::string& html) {
    auto grab_quoted = [&](size_t from, char q) -> std::string {
        size_t p = html.find(q, from);
        if (p == std::string::npos) return {};
        size_t e = html.find(q, p + 1);
        if (e == std::string::npos) return {};
        return html_unescape(html.substr(p + 1, e - p - 1));
    };
    size_t p = html.find("msg_title");
    if (p != std::string::npos) {
        size_t q1 = html.find('\'', p);
        size_t q2 = html.find('"', p);
        if (q1 != std::string::npos && (q2 == std::string::npos || q1 < q2)) {
            std::string t = grab_quoted(q1, '\'');
            if (!t.empty()) return t;
        } else if (q2 != std::string::npos) {
            std::string t = grab_quoted(q2, '"');
            if (!t.empty()) return t;
        }
    }
    p = html.find("og:title");
    if (p != std::string::npos) {
        size_t c = html.find("content=", p);
        if (c != std::string::npos) {
            char q = html[c + 8];
            if (q == '"' || q == '\'') {
                std::string t = grab_quoted(c + 8, q);
                if (!t.empty()) return t;
            }
        }
    }
    size_t ts = html.find("<title");
    if (ts != std::string::npos) {
        size_t a = html.find('>', ts);
        size_t b = html.find("</title>", a);
        if (a != std::string::npos && b != std::string::npos && b > a + 1) {
            return html_unescape(html.substr(a + 1, b - a - 1));
        }
    }
    return {};
}

// 标题缓存：同篇文章只抓一次，容量受限，超限整体清空（简单、无碎片）
struct TitleCache {
    struct Entry { std::string title; SteadyClock::time_point ts; };
    std::unordered_map<std::string, Entry> m;
    std::mutex mtx;
    static constexpr size_t MAX = 4096;
    bool get(const std::string& key, std::string& out) {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = m.find(key);
        if (it == m.end()) return false;
        out = it->second.title;
        return true;
    }
    void put(const std::string& key, const std::string& title) {
        std::lock_guard<std::mutex> lk(mtx);
        if (m.size() >= MAX) m.clear();
        m[key] = { title, SteadyClock::now() };
    }
};

static TitleCache g_title_cache;

static std::string fetch_title(const std::string& url) {
    std::string key = article_key(url);
    std::string cached;
    if (g_title_cache.get(key, cached)) return cached;   // 命中缓存，不发起网络请求

    std::string title;
    std::wstring wurl = u2w(url);
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[2048] = {0}, extra[2048] = {0};
    uc.lpszHostName = host;   uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return title;
    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring fullpath = path;
    if (extra[0]) fullpath += extra;   // query string 必须拼回路径

    HttpHandle hSession = HttpHandle(WinHttpOpen(L"WeChatLinkWatcher/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) return title;
    WinHttpSetTimeouts(hSession, 8000, 8000, 8000, 8000);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

    HttpHandle hConn = HttpHandle(WinHttpConnect(hSession, host, INTERNET_DEFAULT_PORT, 0));
    if (!hConn) return title;
    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HttpHandle hReq = HttpHandle(WinHttpOpenRequest(hConn, L"GET", fullpath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!hReq) return title;

    LPCWSTR hdr = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36\r\n"
                  L"Accept-Encoding: identity\r\n"
                  L"Referer: https://mp.weixin.qq.com/\r\n";
    WinHttpAddRequestHeaders(hReq, hdr, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, nullptr)) {
        std::string html;
        DWORD avail = 0, read = 0;
        char buf[8192];
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0 && html.size() < 400000) {
            DWORD toread = (avail < sizeof(buf)) ? avail : sizeof(buf);
            if (!WinHttpReadData(hReq, buf, toread, &read)) break;
            if (read == 0) break;
            html.append(buf, read);
        }
        title = extract_title_from_html(html);
        // 微信验证页约 17KB 且含 verify 标记，此时标题无效，不缓存
        if (!title.empty() && (title == "微信公众平台" || title == "mp.weixin.qq.com")) title.clear();
    }
    g_title_cache.put(key, title);
    return title;
}

// ---------------- 数据源：浏览器历史 ----------------
struct LinkRow { std::string url; std::string title; std::string source; };

static void add_profile(std::vector<std::pair<fs::path, std::string>>& out,
                        const fs::path& root, const char* name) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (auto& it : fs::directory_iterator(root, ec)) {
        if (ec) break;
        fs::path h = it.path() / L"History";
        std::error_code ec2;
        if (fs::exists(h, ec2)) out.emplace_back(h, name);
    }
}

static std::vector<std::pair<fs::path, std::string>> chromelike_profiles() {
    std::vector<std::pair<fs::path, std::string>> out;
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return out;
    fs::path base(local);
    add_profile(out, base / L"Google\\Chrome\\User Data", "Chrome");
    add_profile(out, base / L"Microsoft\\Edge\\User Data", "Edge");
    add_profile(out, base / L"BraveSoftware\\Brave-Browser\\User Data", "Brave");
    add_profile(out, base / L"360se6\\User Data", "360");
    add_profile(out, base / L"Tencent\\QQBrowser\\User Data", "QQ浏览器");
    add_profile(out, base / L"360Chrome\\Chrome\\User Data", "360Chrome");
    return out;
}

static std::vector<fs::path> firefox_profiles() {
    std::vector<fs::path> out;
    wchar_t appdata[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return out;
    fs::path root = fs::path(appdata) / L"Mozilla\\Firefox\\Profiles";
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;
    for (auto& it : fs::directory_iterator(root, ec)) {
        if (ec) break;
        fs::path p = it.path() / L"places.sqlite";
        std::error_code ec2;
        if (fs::exists(p, ec2)) out.push_back(p);
    }
    return out;
}

static std::vector<LinkRow> scan_history(const fs::path& path, const std::string& source, bool firefox) {
    std::vector<LinkRow> rows;
    SqliteDb db;
    int flags = 0x00000001 | 0x00000040;
    std::string p8 = w2u(path.wstring());
    std::string uri = "file:" + p8 + "?immutable=1";
    if (!db.open(uri, flags)) {
        uri = "file:" + p8 + "?mode=ro";
        if (!db.open(uri, flags)) return rows;
    }
    SqliteStmt st;
    const char* q = firefox
        ? "SELECT url, title FROM moz_places WHERE url LIKE '%mp.weixin.qq.com%' "
          "ORDER BY last_visit_date DESC LIMIT 200"
        : "SELECT url, title FROM urls WHERE url LIKE '%mp.weixin.qq.com%' "
          "ORDER BY last_visit_time DESC LIMIT 200";
    if (st.prepare(db.get(), q)) {
        while (st.row()) {
            std::string url = st.text(0);
            std::string title = st.text(1);
            if (!article_key(url).empty()) rows.push_back({ url, title, source });
        }
    }
    return rows;
}

// ---------------- 数据源：全局剪贴板 ----------------
static std::wstring read_clipboard_text() {
    ClipboardGuard guard;
    if (!guard) return L"";
    std::wstring out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        GlobalLockGuard lock(h);
        if (lock.get()) out = (const wchar_t*)lock.get();
    }
    return out;
}

// ---------------- 记录器（两路共用，线程安全，批量落盘） ----------------
struct Recorder {
    std::unordered_set<std::string> known;
    std::mutex mtx;
    std::ofstream csv, jsonl;
    std::vector<std::string> pending_csv, pending_jsonl;
    uint64_t total = 0;
    static constexpr size_t FLUSH_BATCH = 32;   // 攒到 32 行才写盘，减少 IO

    void record(const std::string& url, const std::string& title, const std::string& source) {
        std::string key = article_key(url);
        if (key.empty()) return;
        std::lock_guard<std::mutex> lk(mtx);
        if (known.count(key)) return;
        known.insert(key);
        ++total;
        std::string ts = now_str();
        pending_csv.push_back(ts + "," + csv_escape(title) + "," + url + "," + source + "\r\n");
        pending_jsonl.push_back("{\"time\":\"" + ts + "\",\"title\":\"" + json_escape(title)
                                + "\",\"url\":\"" + url + "\",\"source\":\"" + source + "\"}\n");
        printf("[新 %llu] %s\n", (unsigned long long)total, title.c_str());
        printf("        %s  (%s)\n", url.c_str(), source.c_str());
        fflush(stdout);
        if (pending_csv.size() >= FLUSH_BATCH) flush_locked();
    }

    void flush() {
        std::lock_guard<std::mutex> lk(mtx);
        flush_locked();
    }

private:
    void flush_locked() {   // 调用前须已持锁
        if (!csv.is_open()) return;
        for (auto& line : pending_csv) csv << line;
        for (auto& line : pending_jsonl) jsonl << line;
        csv.flush();
        jsonl.flush();
        pending_csv.clear();
        pending_jsonl.clear();
    }
};

// ---------------- 全局状态 ----------------
static std::atomic<bool> g_stop{false};
Recorder* g_recorder = nullptr;

static BOOL WINAPI ctrl_handler(DWORD type) {
    g_stop = true;
    return TRUE;
}

// ---------------- 历史轮询线程 ----------------
static void history_thread(Recorder& r) {
    while (!g_stop) {
        for (auto& [path, name] : chromelike_profiles()) {
            for (auto& row : scan_history(path, name, false)) r.record(row.url, row.title, row.source);
        }
        for (auto& p : firefox_profiles()) {
            for (auto& row : scan_history(p, "Firefox", true)) r.record(row.url, row.title, row.source);
        }
        r.flush();
        for (int i = 0; i < 20 && !g_stop; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---------------- 剪贴板监听 ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLIPBOARDUPDATE) {
        std::string u8 = w2u(read_clipboard_text());
        for (auto& url : extract_weixin_links(u8)) {
            std::string title = fetch_title(url);
            g_recorder->record(url, title, "剪贴板");
        }
        g_recorder->flush();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- 主程序 ----------------
int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    printf("================================================\n");
    printf("  公众号链接自动记录器 (C++ · 全电脑)\n");
    printf("  浏览器打开文章 = 自动记录；任何地方复制链接 = 自动记录\n");
    printf("================================================\n");

    if (!load_sqlite()) return 1;

    fs::path data_dir = exe_dir() / L"data";
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    fs::path csv_path = data_dir / L"wechat_links.csv";
    fs::path jsonl_path = data_dir / L"wechat_links.jsonl";

    static Recorder rec;
    g_recorder = &rec;
    {
        std::ifstream in(csv_path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            size_t p = line.find("mp.weixin.qq.com");
            if (p != std::string::npos) {
                std::string key = article_key(line.substr(p));
                if (!key.empty()) rec.known.insert(key);
            }
        }
    }
    printf("[已载入 %zu 条历史记录]\n", rec.known.size());

    rec.csv.open(csv_path, std::ios::app | std::ios::binary);
    rec.jsonl.open(jsonl_path, std::ios::app | std::ios::binary);
    if (!rec.csv.is_open()) { printf("[错误] 无法写入记录文件\n"); return 1; }
    {
        std::ifstream probe(csv_path, std::ios::binary);
        char b[3] = {0};
        probe.read(b, 3);
        bool empty = probe.gcount() == 0;
        probe.close();
        if (empty) { rec.csv << "\xEF\xBB\xBF时间,标题,链接,来源\r\n"; rec.csv.flush(); }
    }

    for (auto& [path, name] : chromelike_profiles())
        for (auto& row : scan_history(path, name, false)) rec.known.insert(article_key(row.url));
    for (auto& p : firefox_profiles())
        for (auto& row : scan_history(p, "Firefox", true)) rec.known.insert(article_key(row.url));

    printf("[基线已建立，开始监控……]\n");
    printf("记录文件：%s\n", w2u(csv_path.wstring()).c_str());
    printf("浏览器里打开公众号文章、或任何地方复制链接，都会自动记录。\n");
    printf("按 Ctrl+C 退出。\n\n");
    fflush(stdout);

    const wchar_t* cls = L"WeChatLinkWatcherHiddenWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    if (!RegisterClassW(&wc)) { printf("[错误] 注册窗口类失败\n"); return 1; }
    HWND hwnd = CreateWindowExW(0, cls, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { printf("[错误] 创建窗口失败\n"); return 1; }
    if (!AddClipboardFormatListener(hwnd)) printf("[警告] 剪贴板监听启动失败\n");
    else printf("[剪贴板监听已启动]\n");
    fflush(stdout);

    std::thread th(history_thread, std::ref(rec));
    th.detach();

    MSG msg;
    while (!g_stop) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_stop = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 优雅退出：注销监听、flush 缓冲、关文件
    RemoveClipboardFormatListener(hwnd);
    DestroyWindow(hwnd);
    rec.flush();
    printf("[已退出，缓冲记录已写入]\n");
    return 0;
}

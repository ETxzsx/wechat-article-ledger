
// ==================== HTTP 服务 ====================
static void ServeClient(SOCKET c) {
    string raw; char tmp[8192];
    while (raw.size() < 8 * 1024 * 1024) {
        int n = recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        raw.append(tmp, n);
        size_t he = raw.find("\r\n\r\n");
        if (he != string::npos) {
            size_t cl = 0;
            size_t p = raw.find("Content-Length:");
            if (p != string::npos && p < he) cl = (size_t)atoi(raw.c_str() + p + 15);
            if (raw.size() >= he + 4 + cl) break;
        }
    }
    if (raw.empty()) { closesocket(c); return; }

    size_t e1 = raw.find("\r\n");
    string line = raw.substr(0, e1 == string::npos ? raw.size() : e1);
    string method, path;
    {
        size_t a = line.find(' '), b = line.find(' ', a + 1);
        method = line.substr(0, a);
        path = (a != string::npos && b != string::npos) ? line.substr(a + 1, b - a - 1) : "/";
    }
    string body = BodyOf(raw);
    KLog("req " + method + " " + path + " raw=" + to_string(raw.size()));

    if (path.rfind("/api/", 0) == 0) {
        string resp;
        try {
            resp = HandleApi(method, path, body);
        } catch (const std::exception& e) {
            KLog("api exception " + path + ": " + e.what());
            resp = "{\"ok\":false,\"error\":\"internal error\"}";
        } catch (...) {
            KLog("api unknown exception " + path);
            resp = "{\"ok\":false,\"error\":\"internal error\"}";
        }
        KLog("api " + path + " body=" + to_string(body.size()) + " resp=" + to_string(resp.size()));
        Respond(c, 200, "application/json; charset=utf-8", resp);
        closesocket(c);
        return;
    }

    // 静态文件
    string rel = PathOnly(path);
    if (rel == "/" || rel.empty()) rel = "/index.html";
    wstring full = PathJoin(g_webDir, u2w(rel.substr(1)));
    if (!FileExists(full)) {
        Respond(c, 404, "text/plain; charset=utf-8", "not found");
    } else {
        string data = ReadFileU8(full);
        Respond(c, 200, ContentTypeOf(rel), data);
    }
    closesocket(c);
}

static void ServerThread() {
    KLog("server thread enter");
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { KLog("socket failed err=" + to_string(WSAGetLastError())); return; }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(kPort);
    int br = bind(ls, (sockaddr*)&addr, sizeof(addr));
    if (br != 0) {
        KLog("bind failed err=" + to_string(WSAGetLastError()));
        return;
    }
    listen(ls, 32);
    KLog("server listening on 8780");
    while (true) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        thread([c]() { ServeClient(c); }).detach();
    }
}

// ==================== 浏览器历史（自动抓链接） ====================
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef int (*p_open_v2)(const char*, sqlite3**, int, const char*);
typedef int (*p_prepare_v2)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
typedef int (*p_step)(sqlite3_stmt*);
typedef const unsigned char* (*p_coltext)(sqlite3_stmt*, int);
typedef int (*p_finalize)(sqlite3_stmt*);
typedef int (*p_close)(sqlite3*);
static p_open_v2 s_open = nullptr;
static p_prepare_v2 s_prep = nullptr;
static p_step s_step = nullptr;
static p_coltext s_text = nullptr;
static p_finalize s_fin = nullptr;
static p_close s_close = nullptr;

static bool LoadSqlite() {
    HMODULE h = LoadLibraryW(L"winsqlite3.dll");
    if (!h) h = LoadLibraryW(L"sqlite3.dll");
    if (!h) return false;
    s_open = (p_open_v2)GetProcAddress(h, "sqlite3_open_v2");
    s_prep = (p_prepare_v2)GetProcAddress(h, "sqlite3_prepare_v2");
    s_step = (p_step)GetProcAddress(h, "sqlite3_step");
    s_text = (p_coltext)GetProcAddress(h, "sqlite3_column_text");
    s_fin = (p_finalize)GetProcAddress(h, "sqlite3_finalize");
    s_close = (p_close)GetProcAddress(h, "sqlite3_close");
    return s_open && s_prep && s_step && s_text && s_fin && s_close;
}

static vector<pair<wstring, string>> BrowserProfiles() {
    vector<pair<wstring, string>> out;
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return out;
    struct Spec { const wchar_t* rel; const char* name; };
    Spec specs[] = {
        {L"Google\\Chrome\\User Data", "Chrome"}, {L"Microsoft\\Edge\\User Data", "Edge"},
        {L"BraveSoftware\\Brave-Browser\\User Data", "Brave"}, {L"360se6\\User Data", "360"},
    };
    for (auto& sp : specs) {
        wstring root = PathJoin(local, sp.rel);
        if (!DirExists(root)) continue;
        WIN32_FIND_DATAW fd{};
        HANDLE hf = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (hf == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wstring hp = root + L"\\" + fd.cFileName + L"\\History";
            if (FileExists(hp)) out.push_back({hp, sp.name});
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    return out;
}

static void ScanHistoryOnce() {
    for (auto& prof : BrowserProfiles()) {
        sqlite3* db = nullptr;
        string uri = "file:" + w2u(prof.first) + "?immutable=1";
        if (s_open(uri.c_str(), &db, 0x1 | 0x40, nullptr) != 0) {
            uri = "file:" + w2u(prof.first) + "?mode=ro";
            if (s_open(uri.c_str(), &db, 0x1 | 0x40, nullptr) != 0) continue;
        }
        sqlite3_stmt* st = nullptr;
        if (s_prep(db, "SELECT url,title,last_visit_time FROM urls WHERE url LIKE '%mp.weixin.qq.com%' ORDER BY last_visit_time DESC LIMIT 100", -1, &st, nullptr) == 0) {
            while (s_step(st) == 100) {
                string url = s_text(st, 0) ? (const char*)s_text(st, 0) : "";
                string title = s_text(st, 1) ? (const char*)s_text(st, 1) : "";
                string lvtStr = s_text(st, 2) ? (const char*)s_text(st, 2) : "0";
                if (url.find("/s/") == string::npos && url.find("/s?") == string::npos) continue;
                lock_guard<mutex> lk(g_mu);
                if (FindPool(url)) continue;
                Item it;
                it.id = GenId(); it.url = url; it.title = title; it.source = prof.second;
                it.created = nowStr();
                it.type = "推文";
                it.theme = GuessTheme(title);
                // 历史里的访问时间 -> 发布时间（决定写进哪张月表）
                long long lvt = atoll(lvtStr.c_str());
                if (lvt > 0) {
                    time_t t = (time_t)(lvt / 1000000LL - 11644473600LL);
                    tm tmv{}; localtime_s(&tmv, &t);
                    char b[64]; snprintf(b, sizeof(b), "%d年%d月%d日", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
                    it.date = b;
                }
                g_pool.push_back(it);
            }
            s_fin(st);
        }
        s_close(db);
    }
    lock_guard<mutex> lk(g_mu);
    SaveList(PoolFile(), g_pool);
}

static void HistoryThread() {
    while (true) {
        ScanHistoryOnce();
        this_thread::sleep_for(chrono::seconds(3));
    }
}

static string ReadClipboardText();   // 实现在下面悬浮窗段

// ==================== 动态识别（浏览器历史 + 剪贴板，1 秒刷新） ====================
struct LiveItem { string time, source, url, title; bool autoScanned; };
static vector<LiveItem> g_live;
static set<string> g_liveSeen;
static mutex g_liveMu;

static void LiveAdd(const string& url, const string& title, const string& source) {
    string k = ItemKey(url);
    if (k.empty() || g_liveSeen.count(k)) return;
    g_liveSeen.insert(k);
    LiveItem li{ nowStr(), source, url, title };
    g_live.insert(g_live.begin(), li);
    if (g_live.size() > 300) g_live.resize(300);
    // 没有标题的（剪贴板来源）异步补标题
    if (title.empty()) {
        thread([url, k]() {
            Article a = ParseArticle(HttpGet(url));
            if (a.title.empty()) return;
            lock_guard<mutex> lk(g_liveMu);
            for (auto& it : g_live) if (ItemKey(it.url) == k) it.title = a.title;
        }).detach();
    }
}

static void LiveTick() {
    // 剪贴板
    {
        auto links = ExtractLinks(ReadClipboardText());
        lock_guard<mutex> lk(g_liveMu);
        for (auto& u : links) LiveAdd(u, "", "剪贴板");
    }
    // 浏览器历史（新出现的）
    if (s_open) {
        for (auto& prof : BrowserProfiles()) {
            sqlite3* db = nullptr;
            string uri = "file:" + w2u(prof.first) + "?immutable=1";
            if (s_open(uri.c_str(), &db, 0x1 | 0x40, nullptr) != 0) {
                uri = "file:" + w2u(prof.first) + "?mode=ro";
                if (s_open(uri.c_str(), &db, 0x1 | 0x40, nullptr) != 0) continue;
            }
            sqlite3_stmt* st = nullptr;
            if (s_prep(db, "SELECT url,title FROM urls WHERE url LIKE '%mp.weixin.qq.com%' ORDER BY last_visit_time DESC LIMIT 30", -1, &st, nullptr) == 0) {
                while (s_step(st) == 100) {
                    string url = s_text(st, 0) ? (const char*)s_text(st, 0) : "";
                    string title = s_text(st, 1) ? (const char*)s_text(st, 1) : "";
                    if (url.find("/s/") == string::npos && url.find("/s?") == string::npos) continue;
                    lock_guard<mutex> lk(g_liveMu);
                    LiveAdd(url, title, "浏览器");
                }
                s_fin(st);
            }
            s_close(db);
        }
    }
}

static void LiveThread() {
    while (true) {
        LiveTick();
        this_thread::sleep_for(chrono::milliseconds(1000));
    }
}

static string ApiLive() {
    lock_guard<mutex> lk(g_liveMu);
    string s = "{\"ok\":true,\"items\":[";
    for (size_t i = 0; i < g_live.size(); ++i) {
        if (i) s += ",";
        s += "{\"time\":" + JStr(g_live[i].time) + ",\"source\":" + JStr(g_live[i].source) +
             ",\"url\":" + JStr(g_live[i].url) + ",\"title\":" + JStr(g_live[i].title) + "}";
    }
    s += "],\"total\":" + to_string(g_live.size()) + "}";
    return s;
}

// ==================== 悬浮窗 ====================
static const wchar_t* kFloaterClass = L"WxLedgerFloater";
static RECT g_flRect{};
static bool g_flDragging = false;
static POINT g_flDragStart{};
static RECT g_flWinStart{};

static void PaintFloater(HWND h, HDC dc) {
    RECT rc; GetClientRect(h, &rc);
    int w = rc.right, hgt = rc.bottom;
    HBRUSH bg = CreateSolidBrush(RGB(246, 246, 241));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    // 圆钮
    int cx = w - 36, cy = hgt / 2, r = 30;
    HBRUSH ink = CreateSolidBrush(RGB(28, 30, 27));
    HBRUSH old = (HBRUSH)SelectObject(dc, ink);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, old);
    DeleteObject(ink);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(246, 246, 241));
    HFONT f = CreateFontW(26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei UI");
    HFONT of = (HFONT)SelectObject(dc, f);
    DrawTextW(dc, L"记", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    DeleteObject(f);

    if (g_floaterExpanded) {
        // 两个按钮
        RECT b1{ 8, 12, 168, hgt - 12 };
        HBRUSH seal = CreateSolidBrush(RGB(156, 43, 36));
        FillRect(dc, &b1, seal); DeleteObject(seal);
        SetTextColor(dc, RGB(251, 246, 244));
        HFONT f2 = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei UI");
        HFONT of2 = (HFONT)SelectObject(dc, f2);
        DrawTextW(dc, L"抓取链接", -1, &b1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of2); DeleteObject(f2);

        RECT b2{ 176, 12, 228, hgt - 12 };
        HBRUSH wh = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &b2, wh); DeleteObject(wh);
        FrameRect(dc, &b2, (HBRUSH)GetStockObject(GRAY_BRUSH));
        SetTextColor(dc, RGB(28, 30, 27));
        HFONT f3 = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei UI");
        HFONT of3 = (HFONT)SelectObject(dc, f3);
        DrawTextW(dc, L"退出", -1, &b2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of3); DeleteObject(f3);
    }
}

static string ReadClipboardText() {
    string text;
    if (OpenClipboard(nullptr)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            wchar_t* p = (wchar_t*)GlobalLock(h);
            if (p) { text = w2u(p); GlobalUnlock(h); }
        }
        CloseClipboard();
    }
    return text;
}

static void DoFloaterGrab() {
    string text = ReadClipboardText();
    auto links = ExtractLinks(text);
    if (links.empty()) {
        MessageBoxW(g_floater, L"剪贴板里没有公众号链接。\n请先在微信里「复制链接」，再点「抓取链接」。",
                    L"抓取链接", MB_ICONINFORMATION);
        return;
    }
    thread([links]() {
        for (auto& u : links) {
            Article a = ParseArticle(HttpGet(u));
            Item it = MakeItem(u, a, "悬浮窗");
            lock_guard<mutex> lk(g_mu);
            if (Item* ex = FindPool(u)) { *ex = it; ex->written = false; }
            else g_pool.insert(g_pool.begin(), it);
            SaveList(PoolFile(), g_pool);
        }
    }).detach();
}

// ==================== 剪贴板事件监听（复制即刻记录） ====================
static const wchar_t* kClipClass = L"WxLedgerClipListener";

static LRESULT CALLBACK ClipProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLIPBOARDUPDATE) {
        string text = ReadClipboardText();
        auto links = ExtractLinks(text);
        if (!links.empty()) {
            lock_guard<mutex> lk(g_liveMu);
            for (auto& u : links) LiveAdd(u, "", "剪贴板");
            KLog("clipboard event: " + to_string(links.size()) + " link(s)");
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND CreateClipListener() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = ClipProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClipClass;
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, kClipClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (h && !AddClipboardFormatListener(h)) {
        KLog("AddClipboardFormatListener failed");
        return nullptr;
    }
    KLog(string("clip listener=") + (h ? "1" : "0"));
    return h;
}

static LRESULT CALLBACK FloaterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); PaintFloater(h, dc); EndPaint(h, &ps); return 0; }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            if (!g_floaterExpanded) {
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
            }
            if (g_flDragging) {
                POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                ClientToScreen(h, &p);
                SetWindowPos(h, HWND_TOPMOST, p.x - g_flDragStart.x, p.y - g_flDragStart.y, 0, 0, SWP_NOSIZE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            g_flDragging = true;
            POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            g_flDragStart = p;
            RECT r; GetWindowRect(h, &r); g_flWinStart = r;
            SetCapture(h);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            bool wasDrag = g_flDragging;
            g_flDragging = false;
            POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            int dx = p.x - g_flDragStart.x, dy = p.y - g_flDragStart.y;
            if (wasDrag && (abs(dx) > 4 || abs(dy) > 4)) return 0;  // 拖动不算点击
            if (!g_floaterExpanded) {
                // 展开
                g_floaterExpanded = true;
                SetWindowPos(h, HWND_TOPMOST, g_flWinStart.left - 228, g_flWinStart.top, 300, 72, SWP_NOZORDER);
                SetWindowRgn(h, CreateRoundRectRgn(0, 0, 301, 73, 12, 12), TRUE);
                InvalidateRect(h, nullptr, TRUE);
            } else if (p.x < 172) {
                DoFloaterGrab();
            } else {
                // 收起
                g_floaterExpanded = false;
                SetWindowRgn(h, CreateEllipticRgn(0, 0, 73, 73), TRUE);
                SetWindowPos(h, HWND_TOPMOST, g_flWinStart.left, g_flWinStart.top, 72, 72, SWP_NOZORDER);
                InvalidateRect(h, nullptr, TRUE);
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            if (g_floaterExpanded) {
                // 缩回（延迟一点）
                SetTimer(h, 1, 600, nullptr);
            }
            return 0;
        }
        case WM_TIMER: {
            KillTimer(h, 1);
            POINT pt; GetCursorPos(&pt);
            RECT r; GetWindowRect(h, &r);
            if (!PtInRect(&r, pt) && g_floaterExpanded && !g_flDragging) {
                g_floaterExpanded = false;
                SetWindowRgn(h, CreateEllipticRgn(0, 0, 73, 73), TRUE);
                SetWindowPos(h, HWND_TOPMOST, r.left, r.top, 72, 72, SWP_NOZORDER);
                InvalidateRect(h, nullptr, TRUE);
            }
            return 0;
        }
        case WM_RBUTTONUP:
            ShellExecuteW(nullptr, L"open", L"http://127.0.0.1:8780/", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND CreateFloater() {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = FloaterProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kFloaterClass;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_HAND);
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kFloaterClass, L"推文台账",
                             WS_POPUP, sw - 110, sh * 4 / 10, 72, 72, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!h) return nullptr;
    SetWindowRgn(h, CreateEllipticRgn(0, 0, 73, 73), TRUE);
    ShowWindow(h, SW_HIDE);   // 默认隐藏，由前端按钮或自动显示
    return h;
}

// ==================== 启动 UI ====================
static void LaunchUI() {
    wstring java = PathJoin(g_exeDir, L"java\\jre\\bin\\javaw.exe");
    if (!FileExists(java)) { KLog("javaw not found, skip UI"); return; }
    wstring mp = PathJoin(g_exeDir, L"java\\javafx\\lib");
    wstring cp = PathJoin(g_exeDir, L"java\\classes");
    wstring cmd = L"\"" + java + L"\" --module-path \"" + mp + L"\" --add-modules javafx.controls,javafx.web -cp \"" + cp + L"\" DesktopApp";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, (wchar_t*)cmd.c_str(), nullptr, nullptr, FALSE, 0, nullptr,
                             g_exeDir.c_str(), &si, &pi);
    KLog(string("launch UI=") + (ok ? "1" : "0"));
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
}

// ==================== Excel 写入（COM，保留原表格式） ====================
static const char* kExcelPs = R"PS(param([string]$InputJson)
$ErrorActionPreference = 'Stop'
$req = Get-Content -LiteralPath $InputJson -Raw -Encoding UTF8 | ConvertFrom-Json
$res = @{ ok = $false; error = ''; row = 0 }
$app = $null
try {
    foreach ($id in @('KET.Application','Excel.Application')) {
        try { $app = New-Object -ComObject $id; break } catch {}
    }
    if (-not $app) { throw 'no excel/wps' }
    $app.Visible = $false
    try { $app.DisplayAlerts = $false } catch {}
    $wb = $app.Workbooks.Open($req.path)
    try {
        $ws = $null
        foreach ($s in $wb.Worksheets) { if ($s.Name -eq $req.sheet) { $ws = $s; break } }
        if (-not $ws) {
            $ws = $wb.Worksheets.Add()
            $ws.Name = $req.sheet
            $h2 = @('序号','类型','推文名称','推文链接','推文主题','推文发布时间','文案撰写','运营排版','运营公众号','美工封面','秘书处整合、归档','摄影','审核')
            for ($i=0; $i -lt $h2.Count; $i++) { $ws.Cells.Item(2, $i+1).Value2 = $h2[$i] }
        }
        $used = $ws.UsedRange
        $lastRow = $used.Row + $used.Rows.Count - 1
        if ($lastRow -lt 2) { $lastRow = 2 }
        if ($req.mode -eq 'row') {
            $target = [int]$req.row
            if ($target -lt 3) { $target = $lastRow + 1 }
            else { $ws.Rows.Item($target).Insert(-4121, 0) }
        } else {
            $target = $lastRow + 1
        }
        for ($i=0; $i -lt 13; $i++) {
            $v = ''
            if ($req.values.Count -gt $i) { $v = [string]$req.values[$i] }
            $cell = $ws.Cells.Item($target, $i+1)
            if ($i -eq 0) { $cell.NumberFormat = '0' } else { $cell.NumberFormat = '@' }
            if ($v -ne '') { $cell.Value2 = $v }
        }
        $wb.Save()
        $res.ok = $true
        $res.row = $target
    } finally {
        try { $wb.Close($false) } catch {}
    }
} catch {
    $res.error = [string]$_.Exception.Message
} finally {
    if ($app) { try { $app.Quit() } catch {} }
}
$res | ConvertTo-Json -Compress | Set-Content -LiteralPath $req.result -Encoding UTF8
)PS";

static bool ExcelWriteRow(const wstring& xlsx, const string& sheet, const string& mode, int row,
                          const vector<string>& values, string& err, int& outRow) {
    KLog("excel write: " + w2u(xlsx) + " sheet=" + sheet + " mode=" + mode + " row=" + to_string(row));
    wstring dir = PathJoin(g_cfg.dataDir, L"tmp");
    CreateDirectoryW(dir.c_str(), nullptr);
    wstring script = PathJoin(dir, L"excel_write.ps1");
    wstring reqFile = PathJoin(dir, L"write_req.json");
    wstring resFile = PathJoin(dir, L"write_res.json");
    WriteFileU8(script, string("\xEF\xBB\xBF") + kExcelPs);   // BOM，PowerShell 5.1 才认 UTF-8

    string js = "{\"path\":" + JStr(w2u(xlsx)) + ",\"sheet\":" + JStr(sheet) +
                ",\"mode\":" + JStr(mode) + ",\"row\":" + to_string(row) +
                ",\"result\":" + JStr(w2u(resFile)) + ",\"values\":[";
    for (size_t i = 0; i < values.size(); ++i) { if (i) js += ","; js += JStr(values[i]); }
    js += "]}";
    WriteFileU8(reqFile, js);

    DeleteFileW(resFile.c_str());
    wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script +
                  L"\" -InputJson \"" + reqFile + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, (wchar_t*)cmd.c_str(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, dir.c_str(), &si, &pi)) {
        err = "无法启动 PowerShell";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    string res = ReadFileU8(resFile);
    if (res.size() >= 3 && (unsigned char)res[0] == 0xEF && (unsigned char)res[1] == 0xBB) res = res.substr(3);  // 去 BOM
    if (res.empty()) { err = "Excel 无响应（可能未安装 Excel/WPS 或被占用）"; return false; }
    JP j = JParser(res).Parse();
    bool ok = j && j->t == JVal::OBJ ? j->truthy("ok") : false;
    if (ok) {
        auto rr = j->get("row");
        outRow = (rr && rr->t == JVal::NUM) ? (int)rr->n : 0;
        return true;
    }
    err = (j && j->t == JVal::OBJ) ? j->str("error", "写入失败") : "写入失败";
    return false;
}

// ==================== 选择 Excel ====================
static wstring PickLedgerFile(HWND owner) {
    wchar_t buf[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Excel 台账 (*.xlsx;*.xlsm)\0*.xlsx;*.xlsm\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择要写入的 Excel 台账（13 列：序号/类型/推文名称/…/审核）";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return L"";
    return buf;
}

// ==================== 入口 ====================
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    KLog("kernel start");
    WSADATA wsa;
    int wr = WSAStartup(MAKEWORD(2, 2), &wsa);
    KLog("wsa=" + to_string(wr));
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wstring exe(buf);
    size_t pos = exe.find_last_of(L"\\/");
    g_exeDir = pos == wstring::npos ? L"." : exe.substr(0, pos);
    g_webDir = PathJoin(g_exeDir, L"web");

    LoadConfig();
    if (g_cfg.ledgerPath.empty() || !FileExists(g_cfg.ledgerPath)) {
        wstring picked = PickLedgerFile(nullptr);
        if (!picked.empty()) { g_cfg.ledgerPath = picked; SaveConfig(); KLog("ledger picked: " + w2u(picked)); }
        else KLog("ledger not picked");
    }
    LoadList(PoolFile(), g_pool);
    LoadList(LedgerFile(), g_ledger);
    bool sq = LoadSqlite();
    KLog(string("sqlite loaded=") + (sq ? "1" : "0") + " dataDir=" + w2u(g_cfg.dataDir) +
         " pool=" + to_string(g_pool.size()) + " ledger=" + to_string(g_ledger.size()));

    CreateClipListener();

    thread(ServerThread).detach();
    thread(HistoryThread).detach();
    thread(LiveThread).detach();
    thread([]() {
        Sleep(600);           // 等服务起来
        LaunchUI();
    }).detach();
    KLog("threads started");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

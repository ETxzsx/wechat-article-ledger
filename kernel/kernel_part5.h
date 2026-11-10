
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
        if (s_prep(db, "SELECT url,title FROM urls WHERE url LIKE '%mp.weixin.qq.com%' ORDER BY last_visit_time DESC LIMIT 100", -1, &st, nullptr) == 0) {
            while (s_step(st) == 100) {
                string url = s_text(st, 0) ? (const char*)s_text(st, 0) : "";
                string title = s_text(st, 1) ? (const char*)s_text(st, 1) : "";
                if (url.find("/s/") == string::npos && url.find("/s?") == string::npos) continue;
                lock_guard<mutex> lk(g_mu);
                if (FindPool(url)) continue;
                Item it;
                it.id = GenId(); it.url = url; it.title = title; it.source = prof.second;
                it.created = nowStr();
                if (it.type.empty()) it.type = "推文";
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

static void DoFloaterGrab() {
    // 读剪贴板，提取链接
    string text;
    if (OpenClipboard(g_floater)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            wchar_t* p = (wchar_t*)GlobalLock(h);
            if (p) { text = w2u(p); GlobalUnlock(h); }
        }
        CloseClipboard();
    }
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
    LoadList(PoolFile(), g_pool);
    LoadList(LedgerFile(), g_ledger);
    bool sq = LoadSqlite();
    KLog(string("sqlite loaded=") + (sq ? "1" : "0") + " dataDir=" + w2u(g_cfg.dataDir) +
         " pool=" + to_string(g_pool.size()) + " ledger=" + to_string(g_ledger.size()));

    g_floater = CreateFloater();
    KLog(string("floater=") + (g_floater ? "1" : "0"));

    thread(ServerThread).detach();
    thread(HistoryThread).detach();
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

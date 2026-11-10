
static string ApiLive();   // 实现在 part5

// ==================== 静态文件 ====================
static string ContentTypeOf(const string& name) {
    auto end = [&](const char* s) { size_t n = strlen(s); return name.size() >= n && name.compare(name.size() - n, n, s) == 0; };
    if (end(".html")) return "text/html; charset=utf-8";
    if (end(".css")) return "text/css; charset=utf-8";
    if (end(".js")) return "application/javascript; charset=utf-8";
    if (end(".png")) return "image/png";
    if (end(".jpg") || end(".jpeg")) return "image/jpeg";
    if (end(".ico")) return "image/x-icon";
    if (end(".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

// ==================== HTTP ====================
static void SendAll(SOCKET s, const char* d, size_t n) {
    size_t off = 0;
    while (off < n) {
        int k = send(s, d + off, (int)(n - off), 0);
        if (k <= 0) return;
        off += k;
    }
}
static void Respond(SOCKET c, int code, const string& ct, const string& body) {
    char h[512];
    sprintf_s(h, "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
              code, ct.c_str(), body.size());
    SendAll(c, h, strlen(h));
    SendAll(c, body.data(), body.size());
}
static string BodyOf(const string& raw) {
    size_t p = raw.find("\r\n\r\n");
    return p == string::npos ? "" : raw.substr(p + 4);
}
static string PathOnly(const string& path) {
    size_t p = path.find('?');
    return p == string::npos ? path : path.substr(0, p);
}

// ==================== API ====================
static string ApiState() {
    lock_guard<mutex> lk(g_mu);
    string s = "{\"workbook\":" + JStr(w2u(LedgerXlsx())) + ",\"exists\":true,\"note\":\"\"";
    s += ",\"fields\":[";
    for (int i = 0; i < 13; ++i) {
        if (i) s += ",";
        s += "{\"key\":" + JStr(kKeys[i]) + ",\"col\":" + JStr(string(1, (char)('A' + i))) +
             ",\"head\":" + JStr(kHeads[i]) + ",\"required\":" + (IsRequired(kKeys[i]) ? "true" : "false");
        if (string(kKeys[i]) == "type") s += ",\"options\":[\"视频\",\"贴图\",\"推文\"]";
        s += "}";
    }
    s += "]";
    set<string> months;
    for (auto& it : g_ledger) months.insert(YearMonthOf(it.date));
    if (months.empty()) months.insert("2026年11月");
    s += ",\"sheets\":[";
    bool first = true;
    for (auto& m : months) { if (!first) s += ","; first = false; s += JStr(m); }
    s += "]";
    s += ",\"records\":{";
    first = true;
    for (auto& m : months) {
        int c = 0; for (auto& it : g_ledger) if (YearMonthOf(it.date) == m) ++c;
        if (!first) s += ","; first = false;
        s += JStr(m) + ":" + to_string(c);
    }
    s += "}";
    s += ",\"options\":{\"types\":[\"视频\",\"贴图\",\"推文\"],"
         "\"themes\":[\"红日、翱翔、青凝\",\"学院推文\",\"一站式学生社区\",\"学生组织建设\"],"
         "\"accounts\":[],\"people\":[]}";
    s += ",\"floating\":false";
    s += ",\"poolPending\":" + to_string(PendingCount());
    s += ",\"dataDir\":" + JStr(w2u(g_cfg.dataDir));
    s += "}";
    return s;
}

static string ApiRows() {
    lock_guard<mutex> lk(g_mu);
    set<string> months;
    for (auto& it : g_ledger) months.insert(YearMonthOf(it.date));
    if (months.empty()) months.insert("2026年11月");
    string s = "{\"sheets\":[";
    bool first = true;
    for (auto& m : months) { if (!first) s += ","; first = false; s += JStr(m); }
    s += "],\"rows\":{";
    first = true;
    for (auto& m : months) {
        if (!first) s += ","; first = false;
        s += JStr(m) + ":[";
        bool f2 = true;
        for (auto& it : g_ledger) {
            if (YearMonthOf(it.date) != m) continue;
            if (!f2) s += ","; f2 = false;
            s += "{";
            for (int i = 0; i < 13; ++i) {
                if (i) s += ",";
                s += JStr(kKeys[i]) + ":" + JStr(it.Get(kKeys[i]));
            }
            s += "}";
        }
        s += "]";
    }
    s += "}}";
    return s;
}

static string ApiPool() {
    lock_guard<mutex> lk(g_mu);
    string s = "{\"ok\":true,\"items\":[";
    for (size_t i = 0; i < g_pool.size(); ++i) { if (i) s += ","; s += ItemJsonFull(g_pool[i]); }
    s += "],\"job\":{\"running\":false,\"total\":0,\"done\":0},\"pending\":" + to_string(PendingCount()) +
         ",\"total\":" + to_string(g_pool.size()) + "}";
    return s;
}

static string ApiParse(const string& body) {
    JP j = JParser(body).Parse();
    string url = (j && j->t == JVal::OBJ) ? j->str("url") : "";
    if (url.empty()) return "{\"ok\":false,\"error\":\"请先粘贴公众号文章链接\"}";
    if (url.find("mp.weixin.qq.com") == string::npos) return "{\"ok\":false,\"error\":\"这不是微信公众号链接\"}";
    Article a = ParseArticle(HttpGet(url));
    string s = "{\"ok\":true,\"url\":" + JStr(url) + ",\"title\":" + JStr(a.title) +
               ",\"account\":" + JStr(a.account) + ",\"date\":" + JStr(a.date) +
               ",\"date_cn\":" + JStr(a.date) +
               ",\"sigs\":{";
    bool first = true;
    for (auto& kv : a.sigs) { if (!first) s += ","; first = false; s += JStr(kv.first) + ":" + JStr(kv.second); }
    s += "},\"type_guess\":" + JStr(a.typeGuess) + ",\"theme_guess\":" + JStr(a.themeGuess) + ",\"warnings\":[";
    for (size_t i = 0; i < a.warnings.size(); ++i) { if (i) s += ","; s += JStr(a.warnings[i]); }
    s += "]}";
    return s;
}

static string ApiCapture(const string& body) {
    JP j = JParser(body).Parse();
    string url = (j && j->t == JVal::OBJ) ? j->str("url") : "";
    if (url.empty()) return "{\"ok\":false,\"error\":\"没有识别到公众号链接\"}";
    Article a = ParseArticle(HttpGet(url));
    Item it = MakeItem(url, a, "内核");
    lock_guard<mutex> lk(g_mu);
    Item* ex = FindPool(url);
    bool isNew = false;
    if (ex) { *ex = it; ex->written = false; it = *ex; }
    else { g_pool.insert(g_pool.begin(), it); isNew = true; }
    SaveList(PoolFile(), g_pool);
    string s = "{\"ok\":true,\"item\":" + ItemJsonFull(it) + ",\"new\":" + (isNew ? "true" : "false") +
               ",\"pending\":" + to_string(PendingCount()) + "}";
    return s;
}

static string ApiExtract(const string& body) {
    JP j = JParser(body).Parse();
    string text = (j && j->t == JVal::OBJ) ? j->str("text") : "";
    auto links = ExtractLinks(text);
    string s = "{\"ok\":true,\"count\":" + to_string(links.size()) + ",\"links\":[";
    for (size_t i = 0; i < links.size(); ++i) { if (i) s += ","; s += JStr(links[i]); }
    s += "]}";
    return s;
}

static string ApiPoolRemove(const string& body) {
    JP j = JParser(body).Parse();
    set<string> ids;
    if (j && j->t == JVal::OBJ) {
        auto arr = j->get("ids");
        if (arr && arr->t == JVal::ARR) for (auto& e : arr->arr) if (e && e->t == JVal::STR) ids.insert(e->s);
    }
    lock_guard<mutex> lk(g_mu);
    g_pool.erase(remove_if(g_pool.begin(), g_pool.end(),
        [&](const Item& it) { return ids.count(it.id) > 0; }), g_pool.end());
    SaveList(PoolFile(), g_pool);
    return "{\"ok\":true}";
}

static string ApiPoolClear(const string& body) {
    JP j = JParser(body).Parse();
    bool all = (j && j->t == JVal::OBJ) ? j->truthy("all") : false;
    lock_guard<mutex> lk(g_mu);
    if (all) g_pool.clear();
    else g_pool.erase(remove_if(g_pool.begin(), g_pool.end(), [](const Item& it) { return it.written; }), g_pool.end());
    SaveList(PoolFile(), g_pool);
    return "{\"ok\":true}";
}

static string ApiPoolUpdate(const string& body) {
    JP j = JParser(body).Parse();
    if (!j || j->t != JVal::OBJ) return "{\"ok\":false}";
    string id = j->str("id");
    auto patch = j->get("patch");
    lock_guard<mutex> lk(g_mu);
    for (auto& it : g_pool) {
        if (it.id != id) continue;
        if (patch && patch->t == JVal::OBJ) for (auto& kv : patch->obj) if (kv.second && kv.second->t == JVal::STR) it.Set(kv.first, kv.second->s);
    }
    SaveList(PoolFile(), g_pool);
    return "{\"ok\":true}";
}

static string ApiPoolWrite(const string& body) {
    JP j = JParser(body).Parse();
    set<string> ids; bool hasIds = false;
    if (j && j->t == JVal::OBJ) {
        auto arr = j->get("ids");
        if (arr && arr->t == JVal::ARR) { hasIds = true; for (auto& e : arr->arr) if (e && e->t == JVal::STR) ids.insert(e->s); }
    }
    bool fillSlash = (j && j->t == JVal::OBJ) ? j->truthy("fillSlash") : false;
    int written = 0, skipped = 0;
    lock_guard<mutex> lk(g_mu);
    for (auto& it : g_pool) {
        if (it.written) continue;
        if (hasIds && !ids.count(it.id)) continue;
        Item v = it;
        bool bad = false;
        for (int i = 0; i < 13; ++i) {
            if (IsRequired(kKeys[i]) && v.Get(kKeys[i]).empty()) { bad = true; break; }
        }
        if (bad) {
            if (!fillSlash) { ++skipped; continue; }
            for (int i = 0; i < 13; ++i) {
                if (!IsRequired(kKeys[i]) && string(kKeys[i]) != "seq" && v.Get(kKeys[i]).empty()) v.Set(kKeys[i], "/");
            }
            for (int i = 0; i < 13; ++i) {
                if (IsRequired(kKeys[i]) && v.Get(kKeys[i]).empty()) { bad = true; break; }
            }
            if (bad) { ++skipped; continue; }
        }
        string err;
        if (AppendToLedger(v, err)) { it.written = true; ++written; }
        else ++skipped;
    }
    SaveList(PoolFile(), g_pool);
    return "{\"ok\":" + string(written > 0 ? "true" : "false") + ",\"written\":" + to_string(written) +
           ",\"skipped\":" + to_string(skipped) + ",\"error\":" +
           JStr(written == 0 ? "没有可写入的条目（多半是资料不全）" : "") + "}";
}

static string ApiAppend(const string& body) {
    JP j = JParser(body).Parse();
    if (!j || j->t != JVal::OBJ) return "{\"ok\":false,\"error\":\"没有数据\"}";
    auto vals = j->get("values");
    if (!vals || vals->t != JVal::OBJ) return "{\"ok\":false,\"error\":\"没有数据\"}";
    Item it;
    for (int i = 0; i < 13; ++i) {
        auto v = vals->get(kKeys[i]);
        if (v && v->t == JVal::STR) it.Set(kKeys[i], v->s);
    }
    string err;
    if (!AppendToLedger(it, err)) return "{\"ok\":false,\"error\":\"还有必填项没填\"}";
    return "{\"ok\":true,\"sheet\":\"\",\"row\":" + to_string((int)g_ledger.size() + 1) + ",\"seq\":" + to_string((int)g_ledger.size()) + "}";
}

static string ApiConfig(const string& body) {
    JP j = JParser(body).Parse();
    if (!j || j->t != JVal::OBJ) return "{\"ok\":false}";
    string dir = j->str("dataDir");
    if (!dir.empty()) {
        lock_guard<mutex> lk(g_mu);
        g_cfg.dataDir = u2w(dir);
        MakeDirs(g_cfg.dataDir);
        SaveConfig();
        LoadList(PoolFile(), g_pool);
        LoadList(LedgerFile(), g_ledger);
    }
    return "{\"ok\":true,\"dataDir\":" + JStr(w2u(g_cfg.dataDir)) + "}";
}

static wstring PickFolderDialog(HWND owner, const wchar_t* title) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    wstring out;
    if (pidl) {
        wchar_t buf[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidl, buf)) out = buf;
        CoTaskMemFree(pidl);
    }
    CoUninitialize();
    return out;
}

static string ApiPickFolder() {
    wstring dir = PickFolderDialog(g_floater, L"选择数据存储文件夹（链接池与台账都放这里）");
    if (dir.empty()) return "{\"ok\":false,\"cancelled\":true}";
    lock_guard<mutex> lk(g_mu);
    g_cfg.dataDir = dir;
    MakeDirs(g_cfg.dataDir);
    SaveConfig();
    LoadList(PoolFile(), g_pool);
    LoadList(LedgerFile(), g_ledger);
    return "{\"ok\":true,\"dataDir\":" + JStr(w2u(g_cfg.dataDir)) + "}";
}

// ==================== 路由 ====================
static string HandleApi(const string& method, const string& rawPath, const string& body) {
    string path = PathOnly(rawPath);
    if (path == "/api/state") return ApiState();
    if (path == "/api/live") return ApiLive();
    if (path == "/api/rows") return ApiRows();
    if (path == "/api/pool") return ApiPool();
    if (path == "/api/ping") return "{\"ok\":true,\"pending\":" + to_string(PendingCount()) + "}";
    if (path == "/api/parse") return ApiParse(body);
    if (path == "/api/capture") return ApiCapture(body);
    if (path == "/api/urls/extract") return ApiExtract(body);
    if (path == "/api/pool/remove") return ApiPoolRemove(body);
    if (path == "/api/pool/clear") return ApiPoolClear(body);
    if (path == "/api/pool/update") return ApiPoolUpdate(body);
    if (path == "/api/pool/write") return ApiPoolWrite(body);
    if (path == "/api/pool/fetch") return ApiExtract(body);
    if (path == "/api/append") return ApiAppend(body);
    if (path == "/api/config") return ApiConfig(body);
    if (path == "/api/pick-folder" || path == "/api/pick-file") return ApiPickFolder();
    if (path == "/api/open-file") {
        ShellExecuteW(nullptr, L"open", LedgerXlsx().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return "{\"ok\":true}";
    }
    if (path == "/api/floating/start" || path == "/api/floating/stop")
        return "{\"ok\":false,\"error\":\"悬浮窗已取消，请用「动态识别」页\"}";
    if (path == "/api/preview") return "{\"ok\":true,\"sheet\":\"\",\"row\":" + to_string((int)g_ledger.size() + 2) + ",\"seq\":" + to_string((int)g_ledger.size() + 1) + ",\"occupied\":false}";
    if (path == "/api/undo") return "{\"ok\":false,\"error\":\"暂不支持撤销\"}";
    return "{\"ok\":false,\"error\":\"unknown api\"}";
}

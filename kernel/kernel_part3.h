
// ==================== 配置与持久化 ====================
struct Config {
    wstring dataDir;
    wstring ledgerName = L"推文台账.xlsx";
    wstring ledgerPath;      // 用户选择的 Excel 台账
};
static Config g_cfg;
static mutex g_excelMu;   // 串行化 Excel 写入（COM 单实例）
static HWND g_floater = nullptr;
static bool g_floaterExpanded = false;

static wstring ConfigPath() { return PathJoin(g_exeDir, L"config.json"); }

static void LoadConfig() {
    g_cfg.dataDir = PathJoin(g_exeDir, L"data");
    g_cfg.ledgerName = L"推文台账.xlsx";
    string s = ReadFileU8(ConfigPath());
    if (!s.empty()) {
        JP j = JParser(s).Parse();
        if (j && j->t == JVal::OBJ) {
            string d = j->str("dataDir");
            if (!d.empty()) g_cfg.dataDir = u2w(d);
            string l = j->str("ledgerName");
            if (!l.empty()) g_cfg.ledgerName = u2w(l);
            string lp = j->str("ledgerPath");
            if (!lp.empty()) g_cfg.ledgerPath = u2w(lp);
        }
    }
    MakeDirs(g_cfg.dataDir);
}
static void SaveConfig() {
    string s = "{\"dataDir\":" + JStr(w2u(g_cfg.dataDir)) +
               ",\"ledgerName\":" + JStr(w2u(g_cfg.ledgerName)) +
               ",\"ledgerPath\":" + JStr(w2u(g_cfg.ledgerPath)) + "}";
    WriteFileU8(ConfigPath(), s);
}
static wstring PoolFile() { return PathJoin(g_cfg.dataDir, L"pool.json"); }
static wstring LedgerFile() { return PathJoin(g_cfg.dataDir, L"ledger.json"); }
static wstring LedgerXlsx() {
    if (!g_cfg.ledgerPath.empty()) return g_cfg.ledgerPath;
    return PathJoin(g_cfg.dataDir, g_cfg.ledgerName);
}

static string ItemToJson(const Item& it) {
    string s = "{";
    s += "\"id\":" + JStr(it.id) + ",\"url\":" + JStr(it.url) + ",\"title\":" + JStr(it.title);
    s += ",\"account\":" + JStr(it.account) + ",\"date\":" + JStr(it.date);
    s += ",\"type\":" + JStr(it.type) + ",\"theme\":" + JStr(it.theme);
    s += ",\"writer\":" + JStr(it.writer) + ",\"layout\":" + JStr(it.layout);
    s += ",\"photo\":" + JStr(it.photo) + ",\"review\":" + JStr(it.review);
    s += ",\"art\":" + JStr(it.art) + ",\"secretary\":" + JStr(it.secretary);
    s += ",\"source\":" + JStr(it.source) + ",\"created\":" + JStr(it.created);
    s += ",\"written\":" + string(it.written ? "true" : "false");
    s += "}";
    return s;
}
static Item JsonToItem(const JP& j) {
    Item it;
    it.id = j->str("id"); it.url = j->str("url"); it.title = j->str("title");
    it.account = j->str("account"); it.date = j->str("date"); it.type = j->str("type");
    it.theme = j->str("theme"); it.writer = j->str("writer"); it.layout = j->str("layout");
    it.photo = j->str("photo"); it.review = j->str("review"); it.art = j->str("art");
    it.secretary = j->str("secretary"); it.source = j->str("source"); it.created = j->str("created");
    it.written = j->truthy("written");
    return it;
}
static void LoadList(const wstring& path, vector<Item>& out) {
    out.clear();
    string s = ReadFileU8(path);
    if (s.empty()) return;
    JP j = JParser(s).Parse();
    if (!j || j->t != JVal::ARR) return;
    for (auto& e : j->arr) if (e && e->t == JVal::OBJ) out.push_back(JsonToItem(e));
}
static void SaveList(const wstring& path, const vector<Item>& list) {
    string s = "[";
    for (size_t i = 0; i < list.size(); ++i) { if (i) s += ","; s += ItemToJson(list[i]); }
    s += "]";
    WriteFileU8(path, s);
}

// ==================== 链接提取 ====================
static vector<string> ExtractLinks(const string& text) {
    vector<string> out;
    const string marker = "mp.weixin.qq.com";
    size_t pos = 0;
    while ((pos = text.find(marker, pos)) != string::npos) {
        size_t st = pos;
        if (pos >= 8 && text.compare(pos - 8, 8, "https://") == 0) st = pos - 8;
        else if (pos >= 7 && text.compare(pos - 7, 7, "http://") == 0) st = pos - 7;
        size_t e = pos + marker.size();
        while (e < text.size()) {
            unsigned char c = (unsigned char)text[e];
            if (c >= 0x80) break;
            if (c == ' ' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\r' || c == '\n' || c == '\t') break;
            ++e;
        }
        string url = text.substr(st, e - st);
        if (!url.empty()) {
            if (url.find("/s/") != string::npos || url.find("/s?") != string::npos) {
                if (find(out.begin(), out.end(), url) == out.end()) out.push_back(url);
            }
        }
        pos = e;
    }
    return out;
}

// ==================== 池操作 ====================
static Item* FindPool(const string& url) {
    string k = ItemKey(url);
    for (auto& it : g_pool) if (ItemKey(it.url) == k) return &it;
    return nullptr;
}
static int PendingCount() { int n = 0; for (auto& it : g_pool) if (!it.written) ++n; return n; }

static string MissingJson(const Item& it) {
    string s = "["; bool first = true;
    for (int i = 0; i < 13; ++i) {
        if (!IsRequired(kKeys[i])) continue;
        if (!it.Get(kKeys[i]).empty()) continue;
        if (!first) s += ","; first = false;
        s += JStr(kHeads[i]);
    }
    s += "]";
    return s;
}

static string ItemJsonFull(const Item& it) {
    string s = "{";
    s += "\"id\":" + JStr(it.id) + ",\"url\":" + JStr(it.url) + ",\"title\":" + JStr(it.title);
    s += ",\"account\":" + JStr(it.account) + ",\"date\":" + JStr(it.date);
    s += ",\"type\":" + JStr(it.type) + ",\"theme\":" + JStr(it.theme);
    s += ",\"writer\":" + JStr(it.writer) + ",\"layout\":" + JStr(it.layout);
    s += ",\"photo\":" + JStr(it.photo) + ",\"review\":" + JStr(it.review);
    s += ",\"art\":" + JStr(it.art) + ",\"secretary\":" + JStr(it.secretary);
    s += ",\"source\":" + JStr(it.source) + ",\"created\":" + JStr(it.created);
    s += ",\"status\":" + JStr(it.title.empty() ? "partial" : "parsed");
    s += ",\"written\":" + string(it.written ? "true" : "false");
    s += ",\"missing\":" + MissingJson(it);
    s += "}";
    return s;
}

static Item MakeItem(const string& url, const Article& a, const string& source) {
    Item it;
    it.id = GenId(); it.url = url; it.title = a.title; it.account = a.account; it.date = a.date;
    it.type = a.typeGuess; it.theme = a.themeGuess;
    auto p = a.sigs.find("writer"); if (p != a.sigs.end()) it.writer = p->second;
    p = a.sigs.find("layout"); if (p != a.sigs.end()) it.layout = p->second;
    p = a.sigs.find("photo"); if (p != a.sigs.end()) it.photo = p->second;
    p = a.sigs.find("review"); if (p != a.sigs.end()) it.review = p->second;
    p = a.sigs.find("art"); if (p != a.sigs.end()) it.art = p->second;
    p = a.sigs.find("secretary"); if (p != a.sigs.end()) it.secretary = p->second;
    it.source = source; it.created = nowStr();
    return it;
}

static bool ExcelWriteRow(const wstring& xlsx, const string& sheet, const string& mode, int row,
                          const vector<string>& values, string& err, int& outRow);   // 实现在 part5

// ==================== 写台账 ====================
static bool AppendToLedger(Item it, string& err, const string& mode = "append", int row = 0) {
    if (!FileExists(LedgerXlsx())) { err = "未选择台账 Excel，请先点「换一个文件」选择"; return false; }
    vector<string> miss;
    for (int i = 0; i < 13; ++i) {
        if (IsRequired(kKeys[i]) && it.Get(kKeys[i]).empty()) miss.push_back(kHeads[i]);
    }
    if (!miss.empty()) { err = "资料不全"; return false; }
    vector<string> vals;
    for (int i = 0; i < 13; ++i) vals.push_back(it.Get(kKeys[i]));
    int seq = 1;
    {
        lock_guard<mutex> lk(g_mu);
        int sameMonth = 0; string ym = YearMonthOf(it.date);
        for (auto& r : g_ledger) if (YearMonthOf(r.date) == ym) ++sameMonth;
        seq = sameMonth + 1;
    }
    vals[0] = to_string(seq);

    int outRow = 0;
    string wErr;
    {
        lock_guard<mutex> el(g_excelMu);   // COM 单实例，必须串行
        if (!ExcelWriteRow(LedgerXlsx(), YearMonthOf(it.date), mode, row, vals, wErr, outRow)) {
            KLog("excel write failed: " + wErr);
            err = wErr;
            return false;
        }
    }
    {
        lock_guard<mutex> lk(g_mu);
        Item rec = it;
        rec.seqValue = vals[0];
        rec.wsheet = YearMonthOf(rec.date); rec.wrow = outRow;
        rec.written = true;
        g_ledger.push_back(rec);
        SaveList(LedgerFile(), g_ledger);
    }
    KLog("ledger appended to excel row=" + to_string(outRow));
    return true;
}


// ==================== 文章抓取与解析 ====================
struct Article {
    string title, account, date;
    map<string,string> sigs;
    string typeGuess = "推文", themeGuess = "学院推文";
    vector<string> warnings;
};

static string HttpGet(const string& url, int* outStatus = nullptr, bool follow = true) {
    string html;
    wstring wurl = u2w(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[2048]{}, extra[2048]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return html;

    HINTERNET ses = WinHttpOpen(L"WeChatLedgerKernel/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return html;
    WinHttpSetTimeouts(ses, 10000, 10000, 15000, 15000);
    DWORD rp = follow ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(ses, WINHTTP_OPTION_REDIRECT_POLICY, &rp, sizeof(rp));

    wstring full = path;
    if (extra[0]) full += extra;
    HINTERNET con = WinHttpConnect(ses, host, INTERNET_DEFAULT_PORT, 0);
    HINTERNET req = nullptr;
    if (con) {
        DWORD fl = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        req = WinHttpOpenRequest(con, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, fl);
    }
    if (req) {
        LPCWSTR hdr =
            L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            L"(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36\r\n"
            L"Accept-Encoding: identity\r\nReferer: https://mp.weixin.qq.com/\r\n";
        WinHttpAddRequestHeaders(req, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD st = 0, sz = sizeof(st);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
            if (outStatus) *outStatus = (int)st;
            DWORD avail = 0, rd = 0; char buf[8192];
            while (WinHttpQueryDataAvailable(req, &avail) && avail > 0 && html.size() < 6 * 1024 * 1024) {
                DWORD want = avail < sizeof(buf) ? avail : sizeof(buf);
                if (!WinHttpReadData(req, buf, want, &rd) || rd == 0) break;
                html.append(buf, rd);
            }
        }
        WinHttpCloseHandle(req);
    }
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return html;
}

static string FindBetween(const string& s, const string& a, const string& b, size_t from = 0) {
    size_t p = s.find(a, from);
    if (p == string::npos) return "";
    p += a.size();
    size_t e = s.find(b, p);
    if (e == string::npos) return "";
    return s.substr(p, e - p);
}

static string UnescapeXml(const string& s) {
    string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t e = s.find(';', i);
            if (e != string::npos && e - i <= 8) {
                string ent = s.substr(i + 1, e - i - 1);
                if (ent == "amp") o += '&';
                else if (ent == "lt") o += '<';
                else if (ent == "gt") o += '>';
                else if (ent == "quot") o += '"';
                else if (ent == "apos") o += '\'';
                else if (ent == "nbsp") o += ' ';
                else o += s.substr(i, e - i + 1);
                i = e; continue;
            }
        }
        o += s[i];
    }
    return o;
}

static void StripTags(string& s) {
    string o; bool in = false;
    for (char c : s) {
        if (c == '<') in = true;
        else if (c == '>') in = false;
        else if (!in) o += c;
    }
    s = Trim(o);
    if (s.size() > 200) {
        size_t k = 200;
        while (k > 0 && (s[k] & 0xC0) == 0x80) --k;
        s = s.substr(0, k);
    }
}

static string GuessTheme(const string& title) {
    if (title.find("一站式") != string::npos || title.find("社区") != string::npos) return "一站式学生社区";
    if (title.find("学生会") != string::npos || title.find("团委") != string::npos ||
        title.find("换届") != string::npos || title.find("招新") != string::npos) return "学生组织建设";
    if (title.find("翱翔") != string::npos || title.find("青凝") != string::npos ||
        title.find("红日") != string::npos || title.find("答辩") != string::npos) return "红日、翱翔、青凝";
    return "学院推文";
}

static Article ParseArticle(const string& rawHtml) {
    Article a;
    string html = rawHtml;
    // 微信把 < > & " 转义成 \x3c 等
    const char* ents[] = {"\\x3c","\\x3C","\\x3e","\\x3E","\\x26","\\x22","\\x27"};
    const char* reps[] = {"<","<",">",">","&","\"","'"};
    for (int i = 0; i < 7; ++i) {
        size_t p;
        while ((p = html.find(ents[i])) != string::npos) html.replace(p, strlen(ents[i]), reps[i]);
    }

    a.title = FindBetween(html, "var msg_title = \"", "\"");
    if (a.title.empty()) a.title = FindBetween(html, "var msg_title = '", "'");
    if (a.title.empty()) a.title = FindBetween(html, "property=\"og:title\" content=\"", "\"");
    if (a.title.empty()) a.title = FindBetween(html, "<title>", "</title>");
    StripTags(a.title);
    if (a.title == "微信公众平台") a.title.clear();

    a.account = FindBetween(html, "var nickname = \"", "\"");
    if (a.account.empty()) a.account = FindBetween(html, "id=\"js_name\"", "</a>");
    if (a.account.size() > 60) a.account = a.account.substr(0, 60);
    StripTags(a.account);

    string ct = FindBetween(html, "var ct = \"", "\"");
    if (ct.size() >= 10) {
        time_t t = (time_t)_atoi64(ct.substr(0, 10).c_str());
        tm tmv{}; localtime_s(&tmv, &t);
        char b[64]; sprintf_s(b, "%d年%d月%d日", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        a.date = b;
    }

    struct SigRule { const char* words; const char* key; };
    SigRule rules[] = {
        {"图文来源", "writer"}, {"供稿", "writer"}, {"文案", "writer"}, {"文字", "writer"},
        {"排版", "layout"}, {"编辑", "layout"},
        {"摄影", "photo"}, {"图片", "photo"},
        {"审核", "review"}, {"责编", "review"}, {"终审", "review"},
        {"封面", "art"}, {"美编", "art"}, {"设计", "art"},
        {"归档", "secretary"}, {"整理", "secretary"},
    };
    for (auto& r : rules) {
        string w = r.words;
        size_t p = html.find(w);
        if (p == string::npos) continue;
        // 后面 3 个字符内要有分隔符
        size_t q = p + w.size();
        size_t sep = html.find_first_of("：:|", q);
        if (sep == string::npos || sep > q + 3) continue;
        string v = html.substr(sep + 1, 40);
        size_t e = v.find_first_of("<|\n\r");
        if (e != string::npos) v = v.substr(0, e);
        v = Trim(v);
        if (!v.empty() && v.size() <= 30 && a.sigs.count(r.key) == 0) a.sigs[r.key] = v;
    }

    a.typeGuess = (html.find("<video") != string::npos || html.find("wxv_") != string::npos) ? "视频" : "推文";
    a.themeGuess = GuessTheme(a.title);
    if (a.title.empty()) a.warnings.push_back("没能读到标题，可能遇到微信验证页，请手填。");
    return a;
}

// ==================== xlsx 生成（store 模式 zip，不依赖第三方库） ====================
static uint32_t g_crcTable[256];
static bool g_crcInit = false;
static void CrcInit() {
    if (g_crcInit) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crcTable[i] = c;
    }
    g_crcInit = true;
}
static uint32_t Crc32(const void* d, size_t n) {
    CrcInit();
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)d;
    for (size_t i = 0; i < n; ++i) c = g_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct ZipEnt { string name; uint32_t crc; uint32_t size; uint32_t offset; };

static void Put16(string& b, uint16_t v) { b.push_back((char)(v & 0xFF)); b.push_back((char)(v >> 8)); }
static void Put32(string& b, uint32_t v) {
    b.push_back((char)(v & 0xFF)); b.push_back((char)((v >> 8) & 0xFF));
    b.push_back((char)((v >> 16) & 0xFF)); b.push_back((char)((v >> 24) & 0xFF));
}

static string BuildZip(const vector<pair<string,string>>& files) {
    string out; vector<ZipEnt> ents;
    for (auto& f : files) {
        ZipEnt e; e.name = f.first;
        e.crc = Crc32(f.second.data(), f.second.size());
        e.size = (uint32_t)f.second.size();
        e.offset = (uint32_t)out.size();
        Put32(out, 0x04034b50); Put16(out, 20); Put16(out, 0x0800); Put16(out, 0); Put16(out, 0); Put16(out, 0);
        Put32(out, e.crc); Put32(out, e.size); Put32(out, e.size);
        Put16(out, (uint16_t)e.name.size()); Put16(out, 0);
        out += e.name; out += f.second;
        ents.push_back(e);
    }
    uint32_t cdStart = (uint32_t)out.size();
    for (auto& e : ents) {
        Put32(out, 0x02014b50); Put16(out, 20); Put16(out, 20); Put16(out, 0x0800); Put16(out, 0); Put16(out, 0); Put16(out, 0);
        Put32(out, e.crc); Put32(out, e.size); Put32(out, e.size);
        Put16(out, (uint16_t)e.name.size()); Put16(out, 0); Put16(out, 0); Put16(out, 0); Put16(out, 0); Put32(out, 0);
        Put32(out, e.offset); out += e.name;
    }
    uint32_t cdSize = (uint32_t)out.size() - cdStart;
    Put32(out, 0x06054b50); Put16(out, 0); Put16(out, 0);
    Put16(out, (uint16_t)ents.size()); Put16(out, (uint16_t)ents.size());
    Put32(out, cdSize); Put32(out, cdStart); Put16(out, 0);
    return out;
}

static string XmlEsc(const string& s) {
    string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}

static int ColIndex(const string& cell) {  // "A1" -> 0
    int n = 0;
    for (char c : cell) { if (c >= 'A' && c <= 'Z') n = n * 26 + (c - 'A' + 1); else break; }
    return n - 1;
}
static string CellRef(int row, int col) {
    string s; int c = col;
    do { s.insert(s.begin(), (char)('A' + c % 26)); c = c / 26 - 1; } while (c >= 0);
    return s + to_string(row);
}
static bool IsShort(const string& s) {
    // 纯数字或很短，用数字/字符串
    if (s.empty()) return true;
    for (char c : s) if (!isdigit((unsigned char)c)) return false;
    return true;
}

static string BuildSheetXml(const vector<Item>& rows) {
    string x = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    x += "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
    x += "<cols><col min=\"1\" max=\"1\" width=\"6\"/><col min=\"2\" max=\"2\" width=\"10\"/><col min=\"3\" max=\"3\" width=\"70\"/>";
    x += "<col min=\"4\" max=\"4\" width=\"55\"/><col min=\"5\" max=\"5\" width=\"18\"/><col min=\"6\" max=\"6\" width=\"16\"/>";
    x += "<col min=\"7\" max=\"13\" width=\"14\"/></cols><sheetData>";
    // 表头
    x += "<row r=\"1\">";
    for (int c = 0; c < 13; ++c) {
        x += "<c r=\"" + CellRef(1, c) + "\" t=\"inlineStr\"><is><t>" + XmlEsc(kHeads[c]) + "</t></is></c>";
    }
    x += "</row>";
    int r = 2;
    for (auto& it : rows) {
        x += "<row r=\"" + to_string(r) + "\">";
        for (int c = 0; c < 13; ++c) {
            string v = it.Get(kKeys[c]);
            if (c == 0 && v.empty()) v = to_string(r - 1);
            if (v.empty()) continue;
            string ref = CellRef(r, c);
            if (c == 0 && IsShort(v)) x += "<c r=\"" + ref + "\"><v>" + v + "</v></c>";
            else x += "<c r=\"" + ref + "\" t=\"inlineStr\"><is><t>" + XmlEsc(v) + "</t></is></c>";
        }
        x += "</row>"; ++r;
    }
    x += "</sheetData></worksheet>";
    return x;
}

static string YearMonthOf(const string& date) {
    // 2026年11月8日 -> 2026年11月
    size_t y = date.find("年"), m = date.find("月");
    if (y == string::npos || m == string::npos || m < y) return "台账";
    string yy = date.substr(0, y), mm = date.substr(y + 3, m - y - 3);
    return yy + "年" + mm + "月";
}

static bool BuildLedgerXlsx(const wstring& outPath, const vector<Item>& rows) {
    KLog("xlsx build enter rows=" + to_string(rows.size()));
    // 按月份分组
    vector<string> sheets; map<string, vector<Item>> byMonth;
    for (auto& it : rows) {
        string m = YearMonthOf(it.date);
        if (byMonth.find(m) == byMonth.end()) sheets.push_back(m);
        byMonth[m].push_back(it);
    }
    sort(sheets.begin(), sheets.end());
    if (sheets.empty()) sheets.push_back("2026年11月");

    vector<pair<string,string>> files;
    string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>";
    for (size_t i = 0; i < sheets.size(); ++i) {
        char b[512]; snprintf(b, sizeof(b),
            "<Override PartName=\"/xl/worksheets/sheet%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>",
            (int)i + 1);
        ct += b;
    }
    ct += "</Types>";
    files.push_back({"[Content_Types].xml", ct});

    files.push_back({"_rels/.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>"});

    string wb = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>";
    for (size_t i = 0; i < sheets.size(); ++i) {
        char b[512]; snprintf(b, sizeof(b), "<sheet name=\"%s\" sheetId=\"%d\" r:id=\"rId%d\"/>",
                               XmlEsc(sheets[i]).c_str(), (int)i + 1, (int)i + 1);
        wb += b;
    }
    wb += "</sheets></workbook>";
    files.push_back({"xl/workbook.xml", wb});

    string rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                  "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (size_t i = 0; i < sheets.size(); ++i) {
        char b[512]; snprintf(b, sizeof(b),
            "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%d.xml\"/>",
            (int)i + 1, (int)i + 1);
        rels += b;
    }
    rels += "</Relationships>";
    files.push_back({"xl/_rels/workbook.xml.rels", rels});

    for (size_t i = 0; i < sheets.size(); ++i) {
        char b[64]; snprintf(b, sizeof(b), "xl/worksheets/sheet%d.xml", (int)i + 1);
        files.push_back({b, BuildSheetXml(byMonth[sheets[i]])});
    }

    KLog("xlsx files=" + to_string(files.size()) + " sheets=" + to_string(sheets.size()));
    string zip = BuildZip(files);
    KLog("xlsx zip=" + to_string(zip.size()));
    ofstream f(wstring(outPath), ios::binary | ios::trunc);
    if (!f) { KLog("xlsx open failed"); return false; }
    f.write(zip.data(), (streamsize)zip.size());
    f.close();
    KLog("xlsx written " + w2u(outPath));
    return true;
}

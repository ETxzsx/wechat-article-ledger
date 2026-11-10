// kernel.cpp —— 推文台账内核
// C++ 负责：HTTP 服务、链接抓取、文章解析、链接池、xlsx 台账、悬浮窗、配置
// Java 只负责窗口 UI（JavaFX WebView 加载本服务页面）
#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <windowsx.h>
#include <memory>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

using namespace std;
typedef unsigned char u8;

static const int kPort = 8780;
static wstring g_exeDir;      // exe 所在目录
static wstring g_dataDir;     // 数据目录（可配置）
static wstring g_webDir;      // web 静态目录
static mutex g_mu;

// ==================== 基础 ====================
static string w2u(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static wstring u2w(const string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static string nowStr() {
    time_t t = time(nullptr); tm tmv{}; localtime_s(&tmv, &t);
    char b[32]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
    return b;
}
static wstring PathJoin(const wstring& a, const wstring& b) {
    if (a.empty()) return b;
    wstring s = a;
    if (s.back() != L'\\' && s.back() != L'/') s += L'\\';
    return s + b;
}
static bool FileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool DirExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static void MakeDirs(const wstring& p) {
    if (DirExists(p)) return;
    size_t pos = p.find_last_of(L"\\/");
    if (pos != wstring::npos && pos > 2) MakeDirs(p.substr(0, pos));
    CreateDirectoryW(p.c_str(), nullptr);
}
static string ReadFileU8(const wstring& p) {
    ifstream f(wstring(p), ios::binary);
    if (!f) return "";
    stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static void WriteFileU8(const wstring& p, const string& s) {
    ofstream f(wstring(p), ios::binary | ios::trunc);
    if (!f) return;
    f.write(s.data(), (streamsize)s.size());
}
static string Trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ==================== 极简 JSON ====================
struct JVal;
typedef shared_ptr<JVal> JP;
struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double n = 0;
    string s;
    vector<JP> arr;
    map<string, JP> obj;
    static JP Str(const string& v) { auto p = make_shared<JVal>(); p->t = STR; p->s = v; return p; }
    static JP Num(double v) { auto p = make_shared<JVal>(); p->t = NUM; p->n = v; return p; }
    static JP Bool(bool v) { auto p = make_shared<JVal>(); p->t = BOOL; p->b = v; return p; }
    static JP Arr() { auto p = make_shared<JVal>(); p->t = ARR; return p; }
    static JP Obj() { auto p = make_shared<JVal>(); p->t = OBJ; return p; }
    JP get(const string& k) const { auto it = obj.find(k); return it == obj.end() ? nullptr : it->second; }
    string str(const string& k, const string& d = "") const {
        auto v = get(k); return (v && v->t == STR) ? v->s : d;
    }
    bool truthy(const string& k, bool d = false) const {
        auto v = get(k); return (v && v->t == BOOL) ? v->b : d;
    }
};

static string EscapeJson(const string& s) {
    string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
                else o.push_back((char)c);
        }
    }
    return o;
}

static string JStr(const string& s) { return "\"" + EscapeJson(s) + "\""; }

class JParser {
public:
    explicit JParser(const string& s) : src(s) {}
    JP Parse() { skip(); JP v = value(); return v ? v : JVal::Obj(); }
private:
    const string& src; size_t i = 0;
    void skip() { while (i < src.size() && isspace((unsigned char)src[i])) ++i; }
    JP value() {
        skip();
        if (i >= src.size()) return nullptr;
        char c = src[i];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return JVal::Str(stringLit());
        if (src.compare(i, 4, "true") == 0) { i += 4; return JVal::Bool(true); }
        if (src.compare(i, 5, "false") == 0) { i += 5; return JVal::Bool(false); }
        if (src.compare(i, 4, "null") == 0) { i += 4; return make_shared<JVal>(); }
        return number();
    }
    JP object() {
        auto o = JVal::Obj(); ++i; skip();
        if (i < src.size() && src[i] == '}') { ++i; return o; }
        while (i < src.size()) {
            skip();
            string k = stringLit();
            skip(); if (i < src.size() && src[i] == ':') ++i;
            JP v = value();
            if (v) o->obj[k] = v;
            skip();
            if (i < src.size() && src[i] == ',') { ++i; continue; }
            if (i < src.size() && src[i] == '}') { ++i; break; }
            break;
        }
        return o;
    }
    JP array() {
        auto a = JVal::Arr(); ++i; skip();
        if (i < src.size() && src[i] == ']') { ++i; return a; }
        while (i < src.size()) {
            JP v = value();
            if (v) a->arr.push_back(v);
            skip();
            if (i < src.size() && src[i] == ',') { ++i; continue; }
            if (i < src.size() && src[i] == ']') { ++i; break; }
            break;
        }
        return a;
    }
    string stringLit() {
        string o; if (i >= src.size() || src[i] != '"') return o;
        ++i;
        while (i < src.size()) {
            char c = src[i++];
            if (c == '"') break;
            if (c == '\\' && i < src.size()) {
                char e = src[i++];
                switch (e) {
                    case 'n': o.push_back('\n'); break;
                    case 'r': o.push_back('\r'); break;
                    case 't': o.push_back('\t'); break;
                    case 'u':
                        if (i + 4 <= src.size()) {
                            int cp = (int)strtol(src.substr(i, 4).c_str(), nullptr, 16); i += 4;
                            if (cp < 0x80) o.push_back((char)cp);
                            else if (cp < 0x800) { o.push_back((char)(0xC0 | (cp >> 6))); o.push_back((char)(0x80 | (cp & 0x3F))); }
                            else { o.push_back((char)(0xE0 | (cp >> 12))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
                        }
                        break;
                    default: o.push_back(e);
                }
            } else o.push_back(c);
        }
        return o;
    }
    JP number() {
        size_t st = i;
        while (i < src.size() && (isdigit((unsigned char)src[i]) || src[i] == '-' || src[i] == '+' || src[i] == '.' || src[i] == 'e' || src[i] == 'E')) ++i;
        return JVal::Num(atof(src.substr(st, i - st).c_str()));
    }
};

// ==================== 字段与模型 ====================
static const char* kHeads[13] = { "序号","类型","推文名称","推文链接","推文主题","推文发布时间",
                                  "文案撰写","运营排版","运营公众号","美工封面","秘书处整合、归档","摄影","审核" };
static const char* kKeys[13]  = { "seq","type","title","url","theme","date",
                                  "writer","layout","account","art","secretary","photo","review" };
static bool IsRequired(const string& k) {
    return k=="type"||k=="title"||k=="url"||k=="theme"||k=="date"||k=="layout"||k=="account";
}

struct Item {
    string id, url, title, account, date, type, theme;
    string writer, layout, photo, review, art, secretary;
    string source, created;
    string seqValue;
    bool written = false;
    string wsheet; int wrow = 0;
    string Get(const string& k) const {
        if (k=="id") return id; if (k=="url") return url; if (k=="title") return title;
        if (k=="account") return account; if (k=="date") return date; if (k=="type") return type;
        if (k=="theme") return theme; if (k=="writer") return writer; if (k=="layout") return layout;
        if (k=="photo") return photo; if (k=="review") return review; if (k=="art") return art;
        if (k=="secretary") return secretary; if (k=="source") return source; if (k=="created") return created;
        if (k=="seq") return seqValue;
        return "";
    }
    void Set(const string& k, const string& v) {
        if (k=="url") url=v; else if (k=="title") title=v; else if (k=="account") account=v;
        else if (k=="date") date=v; else if (k=="type") type=v; else if (k=="theme") theme=v;
        else if (k=="writer") writer=v; else if (k=="layout") layout=v; else if (k=="photo") photo=v;
        else if (k=="review") review=v; else if (k=="art") art=v; else if (k=="secretary") secretary=v;
    }
};

static vector<Item> g_pool;
static vector<Item> g_ledger;   // 台账记录（含已写入的）

static string ItemKey(const string& url) {
    size_t p = url.find("mp.weixin.qq.com");
    if (p == string::npos) return url;
    string rest = url.substr(p + 15);
    if (rest.compare(0, 3, "/s/") == 0) {
        size_t e = rest.find_first_of("?#");
        string id = (e == string::npos) ? rest.substr(3) : rest.substr(3, e - 3);
        return "s/" + id;
    }
    return url;
}
static string GenId() {
    static int c = 0;
    char b[32]; sprintf_s(b, "%08x%04x", (unsigned)GetTickCount(), ++c);
    return b;
}

static void KLog(const string& s) {
    static mutex lm;
    lock_guard<mutex> lk(lm);
    wstring d = PathJoin(g_exeDir, L"data");
    CreateDirectoryW(d.c_str(), nullptr);
    ofstream f(wstring(PathJoin(d, L"kernel.log")), ios::app);
    if (f) f << nowStr() << "  " << s << "\n";
}


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


static string ApiLive();   // 实现在 part5
static wstring PickLedgerFile(HWND owner);   // 实现在 part5

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
    bool hasLedger = !g_cfg.ledgerPath.empty() && FileExists(g_cfg.ledgerPath);
    string s = "{\"workbook\":" + JStr(w2u(LedgerXlsx())) + ",\"exists\":" + (hasLedger ? "true" : "false") +
               ",\"hasLedger\":" + (hasLedger ? "true" : "false") + ",\"note\":\"\"";
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
    string mode = (j && j->t == JVal::OBJ) ? j->str("mode", "append") : "append";
    int startRow = 0;
    { auto rr = (j && j->t == JVal::OBJ) ? j->get("row") : nullptr; if (rr && rr->t == JVal::NUM) startRow = (int)rr->n; }

    if (!FileExists(LedgerXlsx())) return "{\"ok\":false,\"written\":0,\"skipped\":0,\"error\":\"未选择台账 Excel，请先点「换一个文件」选择\"}";

    // 快照待写条目（纯内存，持锁；缺日期/缺标题的单独统计）
    vector<Item> todo;
    int noDate = 0, incomplete = 0;
    {
        lock_guard<mutex> lk(g_mu);
        for (auto& it : g_pool) {
            if (it.written) continue;
            if (hasIds && !ids.count(it.id)) continue;
            Item v = it;
            if (v.Get("date").empty()) { ++noDate; continue; }
            bool bad = false;
            for (int i = 0; i < 13; ++i) if (IsRequired(kKeys[i]) && v.Get(kKeys[i]).empty()) { bad = true; break; }
            if (bad) {
                if (!fillSlash) { ++incomplete; continue; }
                if (v.Get("theme").empty()) v.Set("theme", GuessTheme(v.Get("title")));
                if (v.Get("type").empty()) v.Set("type", "推文");
                for (int i = 0; i < 13; ++i) {
                    const string k = kKeys[i];
                    if (k == "title" || k == "url" || k == "date" || k == "type" || k == "theme") continue;
                    if (v.Get(k).empty()) v.Set(k, "/");
                }
                if (v.Get("title").empty() || v.Get("url").empty()) { ++incomplete; continue; }
            }
            todo.push_back(v);
        }
    }

    int written = 0;
    string lastErr;
    for (auto& v : todo) {
        string err;
        if (AppendToLedger(v, err, mode, startRow)) {
            ++written;
            lock_guard<mutex> lk(g_mu);
            for (auto& it : g_pool) if (it.id == v.id) { it.written = true; break; }
        } else if (!err.empty() && err != "资料不全") lastErr = err;
    }
    { lock_guard<mutex> lk(g_mu); SaveList(PoolFile(), g_pool); }

    int skipped = noDate + incomplete + ((int)todo.size() - written);
    string e;
    if (written == 0) {
        if (!lastErr.empty()) e = lastErr;
        else if (noDate > 0) e = "有 " + to_string(noDate) + " 条缺「发布时间」（它决定写进哪张月表），请补上或勾选「缺失项自动填 /」后重试";
        else e = "没有可写入的条目：请勾选「缺失项自动填 /」或手动补全字段";
    }
    return "{\"ok\":" + string(written > 0 ? "true" : "false") + ",\"written\":" + to_string(written) +
           ",\"skipped\":" + to_string(skipped) + ",\"error\":" + JStr(e) + "}";
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
    string wmode = j->str("mode", "append");
    int wrow = 0;
    { auto rr = j->get("row"); if (rr && rr->t == JVal::NUM) wrow = (int)rr->n; }
    string err;
    if (!AppendToLedger(it, err, wmode, wrow)) return "{\"ok\":false,\"error\":" + JStr(err.empty() ? "写入失败" : err) + "}";
    if (g_ledger.empty()) return "{\"ok\":false,\"error\":\"写入失败\"}";
    int realRow = g_ledger.empty() ? 0 : g_ledger.back().wrow;
    int realSeq = g_ledger.empty() ? 0 : atoi(g_ledger.back().Get("seq").c_str());
    return "{\"ok\":true,\"sheet\":" + JStr(YearMonthOf(it.date)) + ",\"row\":" + to_string(realRow) +
           ",\"seq\":" + to_string(realSeq) + "}";
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
    if (path == "/api/pick-folder") return ApiPickFolder();
    if (path == "/api/pick-file") {
        wstring picked = PickLedgerFile(g_floater);
        if (picked.empty()) return "{\"ok\":false,\"cancelled\":true}";
        lock_guard<mutex> lk(g_mu);
        g_cfg.ledgerPath = picked;
        SaveConfig();
        return "{\"ok\":true,\"workbook\":" + JStr(w2u(picked)) + "}";
    }
    if (path == "/api/open-file") {
        ShellExecuteW(nullptr, L"open", LedgerXlsx().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return "{\"ok\":true}";
    }
    if (path == "/api/floating/start" || path == "/api/floating/stop")
        return "{\"ok\":false,\"error\":\"悬浮窗已取消，请用「动态识别」页\"}";
    if (path == "/api/preview") {
        JP pj = JParser(body).Parse();
        string date = (pj && pj->t == JVal::OBJ) ? pj->str("date") : "";
        string ym = YearMonthOf(date);
        lock_guard<mutex> lk(g_mu);
        int sameMonth = 0;
        for (auto& r : g_ledger) if (YearMonthOf(r.date) == ym) ++sameMonth;
        return "{\"ok\":true,\"sheet\":" + JStr(ym) + ",\"row\":" + to_string(sameMonth + 3) +
               ",\"seq\":" + to_string(sameMonth + 1) + ",\"occupied\":false}";
    }
    if (path == "/api/undo") return "{\"ok\":false,\"error\":\"暂不支持撤销\"}";
    return "{\"ok\":false,\"error\":\"unknown api\"}";
}


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

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
    bool written = false;
    string wsheet; int wrow = 0;
    string Get(const string& k) const {
        if (k=="id") return id; if (k=="url") return url; if (k=="title") return title;
        if (k=="account") return account; if (k=="date") return date; if (k=="type") return type;
        if (k=="theme") return theme; if (k=="writer") return writer; if (k=="layout") return layout;
        if (k=="photo") return photo; if (k=="review") return review; if (k=="art") return art;
        if (k=="secretary") return secretary; if (k=="source") return source; if (k=="created") return created;
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

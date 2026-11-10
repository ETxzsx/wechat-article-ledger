import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import java.io.*;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.awt.Desktop;
import java.time.*;
import java.time.format.DateTimeFormatter;
import java.util.*;
import java.util.regex.*;

/**
 * 推文台账 · Java 前端服务
 * 托管原 web/ 页面（界面与网页版完全一致），并把 C++ 记录器抓到的链接接进「链接池」。
 * 零第三方依赖：JDK 自带 HttpServer。
 */
public class LedgerFrontend {
    static final int PORT = 8780;
    static Path baseDir, webDir, dataDir, ledgerCsv, poolJson;

    static final String[] HEADS = {
        "序号", "类型", "推文名称", "推文链接", "推文主题", "推文发布时间",
        "文案撰写", "运营排版", "运营公众号", "美工封面", "秘书处整合、归档", "摄影", "审核"
    };
    static final String[] KEYS = {
        "seq", "type", "title", "url", "theme", "date",
        "writer", "layout", "account", "art", "secretary", "photo", "review"
    };
    static final String[] COLS = {
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M"
    };

    static final List<Map<String, Object>> pool = new ArrayList<>();
    static final Object LOCK = new Object();

    public static void main(String[] args) throws Exception {
        // 控制台统一 UTF-8，避免中文乱码
        System.setOut(new PrintStream(new FileOutputStream(FileDescriptor.out), true, StandardCharsets.UTF_8));
        baseDir = Paths.get(System.getProperty("app.dir", ".")).toAbsolutePath().normalize();
        webDir = baseDir.resolve("web");
        dataDir = baseDir.resolve("data").resolve("frontend");
        ledgerCsv = dataDir.resolve("ledger.csv");
        poolJson = dataDir.resolve("pool.json");
        Files.createDirectories(dataDir);

        loadPool();
        importCppLinks();

        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", PORT), 0);
        server.createContext("/api/", LedgerFrontend::api);
        server.createContext("/", LedgerFrontend::staticFile);
        server.setExecutor(java.util.concurrent.Executors.newFixedThreadPool(4));
        server.start();

        String url = "http://127.0.0.1:" + PORT + "/";
        System.out.println("==================================================");
        System.out.println("  推文台账 · Java 前端");
        System.out.println("  界面： " + url);
        System.out.println("  台账： " + ledgerCsv);
        System.out.println("  按 Ctrl+C 退出");
        System.out.println("==================================================");
        if (!"true".equals(System.getProperty("no.browser"))) {
            try {
                if (Desktop.isDesktopSupported()) Desktop.getDesktop().browse(URI.create(url));
            } catch (Exception ignored) {}
        }
    }

    // ---------------- 静态文件 ----------------
    static void staticFile(HttpExchange ex) throws IOException {
        String path = ex.getRequestURI().getPath();
        if (path == null || path.equals("/")) path = "/index.html";
        Path f = webDir.resolve(path.substring(1)).normalize();
        if (!f.startsWith(webDir) || !Files.isRegularFile(f)) {
            send(ex, 404, "text/plain; charset=utf-8", "not found".getBytes(StandardCharsets.UTF_8));
            return;
        }
        String name = f.getFileName().toString().toLowerCase();
        String ct = name.endsWith(".html") ? "text/html; charset=utf-8"
                  : name.endsWith(".css")  ? "text/css; charset=utf-8"
                  : name.endsWith(".js")   ? "application/javascript; charset=utf-8"
                  : name.endsWith(".jpg") || name.endsWith(".jpeg") ? "image/jpeg"
                  : name.endsWith(".png")  ? "image/png"
                  : name.endsWith(".ico")  ? "image/x-icon"
                  : name.endsWith(".xlsx") ? "application/octet-stream"
                  : "application/octet-stream";
        send(ex, 200, ct, Files.readAllBytes(f));
    }

    // ---------------- API 路由 ----------------
    static void api(HttpExchange ex) throws IOException {
        String path = ex.getRequestURI().getPath();
        String method = ex.getRequestMethod();
        String body = readBody(ex);
        try {
            switch (path) {
                case "/api/state":        sendJson(ex, state()); return;
                case "/api/rows":         sendJson(ex, rows()); return;
                case "/api/pool":         sendJson(ex, poolPayload()); return;
                case "/api/ping":         sendJson(ex, map("ok", true, "pending", pendingCount())); return;
                case "/api/parse":        sendJson(ex, parseLink(body)); return;
                case "/api/preview":      sendJson(ex, preview(body)); return;
                case "/api/append":       sendJson(ex, appendRow(body)); return;
                case "/api/capture":      sendJson(ex, capture(body)); return;
                case "/api/urls/extract": sendJson(ex, extractLinks(body)); return;
                case "/api/pool/fetch":   sendJson(ex, fetchPool(body)); return;
                case "/api/pool/write":   sendJson(ex, writePool(body)); return;
                case "/api/pool/remove":  sendJson(ex, removePool(body)); return;
                case "/api/pool/clear":   sendJson(ex, clearPool(body)); return;
                case "/api/pool/update":  sendJson(ex, updatePool(body)); return;
                case "/api/config":       sendJson(ex, map("ok", true, "workbook", ledgerCsv.toString(), "exists", true)); return;
                case "/api/open-file":    sendJson(ex, openFile()); return;
                case "/api/undo":         sendJson(ex, map("ok", false, "error", "Java 版暂不支持撤销（请用 CSV 备份）")); return;
                case "/api/pick-file":    sendJson(ex, map("ok", false, "error", "Java 版使用固定 CSV 台账")); return;
                case "/api/urls/upload":  sendJson(ex, map("ok", false, "error", "Java 版暂不支持上传文件，请粘贴链接")); return;
                case "/api/floating/start": case "/api/floating/stop":
                    sendJson(ex, map("ok", false, "error", "悬浮按钮属于 C++ 记录器，Java 前端不含此功能")); return;
                case "/api/floating/status": sendJson(ex, map("running", false, "pid", null)); return;
                default:
                    sendJson(ex, map("ok", false, "error", "unknown api: " + path));
            }
        } catch (Exception e) {
            sendJson(ex, map("ok", false, "error", String.valueOf(e)));
        }
    }

    // ---------------- 数据 ----------------
    static Map<String, Object> state() {
        List<Map<String, Object>> fields = new ArrayList<>();
        for (int i = 0; i < KEYS.length; i++) {
            Map<String, Object> f = new LinkedHashMap<>();
            f.put("key", KEYS[i]);
            f.put("col", COLS[i]);
            f.put("head", HEADS[i]);
            f.put("required", isRequired(KEYS[i]));
            if (KEYS[i].equals("type")) f.put("options", List.of("视频", "贴图", "推文"));
            fields.add(f);
        }
        Map<String, Object> m = new LinkedHashMap<>();
        m.put("workbook", ledgerCsv.toString());
        m.put("exists", true);
        m.put("note", "");
        m.put("fields", fields);
        m.put("sheets", List.of("台账"));
        Map<String, Object> rec = new LinkedHashMap<>();
        rec.put("台账", ledgerRows().size());
        m.put("records", rec);
        Map<String, Object> opt = new LinkedHashMap<>();
        opt.put("types", List.of("视频", "贴图", "推文"));
        opt.put("themes", List.of("红日、翱翔、青凝", "学院推文", "一站式学生社区", "学生组织建设"));
        opt.put("accounts", new ArrayList<>());
        opt.put("people", new ArrayList<>());
        m.put("options", opt);
        m.put("floating", false);
        m.put("poolPending", pendingCount());
        return m;
    }

    static boolean isRequired(String k) {
        return k.equals("type") || k.equals("title") || k.equals("url") || k.equals("theme")
            || k.equals("date") || k.equals("layout") || k.equals("account");
    }

    static Map<String, Object> rows() {
        List<Map<String, Object>> list = ledgerRows();
        Map<String, Object> inner = new LinkedHashMap<>();
        inner.put("台账", list);
        return map("sheets", List.of("台账"), "rows", inner);
    }

    static List<Map<String, Object>> ledgerRows() {
        List<Map<String, Object>> out = new ArrayList<>();
        if (!Files.isRegularFile(ledgerCsv)) return out;
        try {
            List<String> lines = Files.readAllLines(ledgerCsv, StandardCharsets.UTF_8);
            for (int i = 1; i < lines.size(); i++) {
                String line = lines.get(i);
                if (line.isBlank()) continue;
                List<String> cells = parseCsvLine(line);
                Map<String, Object> m = new LinkedHashMap<>();
                for (int k = 0; k < KEYS.length; k++) {
                    m.put(KEYS[k], k < cells.size() ? cells.get(k) : "");
                }
                m.put("row", i + 1);
                out.add(m);
            }
        } catch (Exception ignored) {}
        return out;
    }

    static Map<String, Object> poolPayload() {
        synchronized (LOCK) {
            List<Map<String, Object>> items = new ArrayList<>();
            for (Map<String, Object> it : pool) {
                Map<String, Object> x = new LinkedHashMap<>(it);
                x.put("missing", missingOf(it));
                items.add(x);
            }
            return map("ok", true, "items", items, "job", map("running", false, "total", 0, "done", 0),
                       "pending", pendingCount(), "total", items.size());
        }
    }

    static int pendingCount() {
        synchronized (LOCK) {
            int n = 0;
            for (Map<String, Object> it : pool) if (it.get("written") == null) n++;
            return n;
        }
    }

    static List<String> missingOf(Map<String, Object> it) {
        List<String> miss = new ArrayList<>();
        for (int i = 0; i < KEYS.length; i++) {
            if (!isRequired(KEYS[i])) continue;
            Object v = it.get(KEYS[i]);
            if (v == null || String.valueOf(v).trim().isEmpty()) miss.add(HEADS[i]);
        }
        return miss;
    }

    // ---------------- 链接解析 ----------------
    static Map<String, Object> parseLink(String body) {
        String url = normalize(str(field(body, "url")));
        if (url.isEmpty()) return map("ok", false, "error", "请先粘贴公众号文章链接");
        if (!url.contains("mp.weixin.qq.com")) return map("ok", false, "error", "这不是微信公众号链接");
        try {
            String html = httpGet(url);
            // 微信页面把 < > & " 转义成 \x3c 等，先还原，签名/标题才干净
            html = html.replace("\\x3c", "<").replace("\\x3C", "<")
                       .replace("\\x3e", ">").replace("\\x3E", ">")
                       .replace("\\x26", "&").replace("\\x22", "\"")
                       .replace("\\x27", "'");
            Map<String, Object> r = new LinkedHashMap<>();
            String title = firstMatch(html, "var\\s+msg_title\\s*=\\s*['\"]([^'\"]+)['\"]");
            if (title.isEmpty()) title = firstMatch(html, "<meta\\s+property=\"og:title\"\\s+content=\"([^\"]*)\"");
            if (title.isEmpty()) title = firstMatch(html, "<title>([^<]*)</title>");
            String account = firstMatch(html, "var\\s+nickname\\s*=\\s*['\"]([^'\"]*)['\"]");
            if (account.isEmpty()) account = firstMatch(html, "id=\"js_name\"[^>]*>([^<]*)<");
            String ct = firstMatch(html, "var\\s+ct\\s*=\\s*\"(\\d{10})\"");
            String date = "";
            if (!ct.isEmpty()) {
                LocalDate d = Instant.ofEpochSecond(Long.parseLong(ct)).atZone(ZoneId.systemDefault()).toLocalDate();
                date = d.getYear() + "年" + d.getMonthValue() + "月" + d.getDayOfMonth() + "日";
            }
            Map<String, Object> sig = new LinkedHashMap<>();
            String[] keys = {"writer", "layout", "photo", "review", "art", "secretary"};
            String[] words = {"(?:图文来源|供稿|文案|文字)", "(?:编辑|排版)", "(?:摄影|图片)", "(?:审核|责编|终审)", "(?:封面|美编|设计)", "(?:归档|整理)"};
            for (int i = 0; i < keys.length; i++) {
                String v = firstMatch(html, words[i] + "\\s*[：:|]\\s*([^<\\n]{1,30})");
                if (!v.isEmpty()) {
                    v = v.replaceAll("<[^>]*>", "").replaceAll("\\\\x[0-9a-fA-F]{2}", "").trim();
                    if (!v.isEmpty()) sig.put(keys[i], v);
                }
            }
            r.put("ok", true);
            r.put("url", url);
            r.put("title", title.trim());
            r.put("account", account.trim());
            r.put("date", date);
            r.put("date_cn", date);
            r.put("sigs", sig);
            r.put("type_guess", html.contains("<video") || html.contains("wxv_") ? "视频" : "推文");
            r.put("theme_guess", guessTheme(title));
            r.put("warnings", title.isEmpty() ? List.of("没能读到标题，可能遇到微信验证页，请手填。") : List.of());
            return r;
        } catch (Exception e) {
            return map("ok", false, "error", "抓取失败：" + e.getMessage());
        }
    }

    static String guessTheme(String title) {
        if (title.contains("一站式") || title.contains("社区")) return "一站式学生社区";
        if (title.contains("学生会") || title.contains("团委") || title.contains("换届") || title.contains("招新")) return "学生组织建设";
        if (title.contains("翱翔") || title.contains("青凝") || title.contains("红日") || title.contains("答辩")) return "红日、翱翔、青凝";
        return "学院推文";
    }

    // ---------------- 池 ----------------
    static Map<String, Object> capture(String body) {
        String url = normalize(str(field(body, "url")));
        if (!url.contains("mp.weixin.qq.com")) return map("ok", false, "error", "没有识别到公众号链接");
        Map<String, Object> parsed = parseLink(body);
        Map<String, Object> it = newItem(url, parsed);
        synchronized (LOCK) {
            Map<String, Object> exist = findPool(url);
            if (exist != null) {
                Object w = exist.get("written");
                exist.putAll(it);
                exist.put("written", w);
                savePool();
                return map("ok", true, "item", exist, "new", false, "pending", pendingCount());
            }
            pool.add(0, it);
            savePool();
            return map("ok", true, "item", it, "new", true, "pending", pendingCount());
        }
    }

    static Map<String, Object> fetchPool(String body) {
        List<Object> urls = (List<Object>) field(body, "urls");
        if (urls == null || urls.isEmpty()) return map("ok", false, "error", "没有可抓取的链接");
        int added = 0;
        for (Object u : urls) {
            String url = normalize(String.valueOf(u));
            if (!url.contains("mp.weixin.qq.com")) continue;
            Map<String, Object> parsed = parseLink(Json.write(map("url", url)));
            Map<String, Object> it = newItem(url, parsed);
            synchronized (LOCK) {
                if (findPool(url) == null) { pool.add(0, it); added++; }
            }
        }
        synchronized (LOCK) { savePool(); }
        return map("ok", true, "queued", added);
    }

    static Map<String, Object> newItem(String url, Map<String, Object> parsed) {
        Map<String, Object> it = new LinkedHashMap<>();
        Map<String, Object> sig = parsed.get("sigs") instanceof Map ? (Map<String, Object>) parsed.get("sigs") : Map.of();
        it.put("id", UUID.randomUUID().toString().substring(0, 10));
        it.put("url", url);
        it.put("title", str(parsed.get("title")));
        it.put("account", str(parsed.get("account")));
        it.put("date", str(parsed.get("date_cn")));
        it.put("type", str(parsed.get("type_guess")));
        it.put("theme", str(parsed.get("theme_guess")));
        it.put("writer", str(sig.get("writer")));
        it.put("layout", str(sig.get("layout")));
        it.put("photo", str(sig.get("photo")));
        it.put("review", str(sig.get("review")));
        it.put("art", str(sig.get("art")));
        it.put("secretary", str(sig.get("secretary")));
        it.put("status", str(parsed.get("title")).isEmpty() ? "partial" : "parsed");
        it.put("source", "java");
        it.put("created", LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss")));
        it.put("written", null);
        return it;
    }

    static Map<String, Object> findPool(String url) {
        String k = keyOf(url);
        for (Map<String, Object> it : pool) if (keyOf(str(it.get("url"))).equals(k)) return it;
        return null;
    }

    static String keyOf(String url) {
        try {
            URI u = URI.create(url.startsWith("http") ? url : "https://" + url);
            String p = u.getPath();
            if (p != null && p.startsWith("/s/")) return p;
            return url;
        } catch (Exception e) { return url; }
    }

    static Map<String, Object> removePool(String body) {
        List<Object> ids = (List<Object>) field(body, "ids");
        synchronized (LOCK) {
            if (ids != null) pool.removeIf(it -> ids.contains(it.get("id")));
            savePool();
        }
        return map("ok", true);
    }

    static Map<String, Object> clearPool(String body) {
        boolean all = Boolean.TRUE.equals(field(body, "all"));
        synchronized (LOCK) {
            if (all) pool.clear();
            else pool.removeIf(it -> it.get("written") != null);
            savePool();
        }
        return map("ok", true);
    }

    static Map<String, Object> updatePool(String body) {
        String id = str(field(body, "id"));
        Map<String, Object> patch = (Map<String, Object>) field(body, "patch");
        synchronized (LOCK) {
            for (Map<String, Object> it : pool) {
                if (id.equals(it.get("id")) && patch != null) {
                    for (String k : patch.keySet()) {
                        if (Arrays.asList(KEYS).contains(k)) it.put(k, patch.get(k));
                    }
                }
            }
            savePool();
        }
        return map("ok", true);
    }

    static Map<String, Object> writePool(String body) {
        List<Object> ids = (List<Object>) field(body, "ids");
        boolean fillSlash = Boolean.TRUE.equals(field(body, "fillSlash"));
        int written = 0, skipped = 0;
        List<Map<String, Object>> skipList = new ArrayList<>();
        synchronized (LOCK) {
            for (Map<String, Object> it : new ArrayList<>(pool)) {
                if (ids != null && !ids.contains(it.get("id"))) continue;
                if (it.get("written") != null) continue;
                List<String> miss = missingOf(it);
                if (!miss.isEmpty() && !fillSlash) {
                    skipped++;
                    skipList.add(map("id", it.get("id"), "title", it.get("title"), "missing", miss));
                    continue;
                }
                Map<String, Object> vals = new LinkedHashMap<>();
                for (int i = 0; i < KEYS.length; i++) {
                    String v = str(it.get(KEYS[i]));
                    if (v.isEmpty() && fillSlash && !isRequired(KEYS[i]) && !KEYS[i].equals("seq")) v = "/";
                    vals.put(KEYS[i], v);
                }
                appendCsv(vals);
                it.put("written", map("sheet", "台账", "row", ledgerRows().size() + 1,
                                      "at", LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss"))));
                written++;
            }
            savePool();
        }
        return map("ok", written > 0, "written", written, "skipped", skipList,
                   "error", written == 0 ? "没有可写入的条目（多半是资料不全）" : null);
    }

    // ---------------- 写入 CSV ----------------
    static Map<String, Object> appendRow(String body) {
        Map<String, Object> values = (Map<String, Object>) field(body, "values");
        if (values == null) return map("ok", false, "error", "没有数据");
        List<String> miss = new ArrayList<>();
        for (int i = 0; i < KEYS.length; i++)
            if (isRequired(KEYS[i]) && str(values.get(KEYS[i])).isEmpty()) miss.add(HEADS[i]);
        if (!miss.isEmpty()) return map("ok", false, "error", "还有必填项没填：" + String.join("、", miss));
        appendCsv(values);
        int row = ledgerRows().size() + 1;
        return map("ok", true, "sheet", "台账", "row", row, "seq", row - 1);
    }

    static synchronized void appendCsv(Map<String, Object> values) {
        try {
            boolean exists = Files.isRegularFile(ledgerCsv) && Files.size(ledgerCsv) > 0;
            StringBuilder sb = new StringBuilder();
            if (!exists) {
                sb.append('\uFEFF');
                sb.append(String.join(",", HEADS)).append("\r\n");
            }
            int seq = ledgerRows().size() + 1;
            List<String> cells = new ArrayList<>();
            for (String k : KEYS) {
                String v = str(values.get(k));
                if (k.equals("seq") && v.trim().isEmpty()) v = String.valueOf(seq);
                cells.add(csvEscape(v));
            }
            sb.append(String.join(",", cells)).append("\r\n");
            Files.write(ledgerCsv, sb.toString().getBytes(StandardCharsets.UTF_8),
                        StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (Exception ignored) {}
    }

    static Map<String, Object> preview(String body) {
        int n = ledgerRows().size();
        return map("ok", true, "sheet", "台账", "sheet_created", false,
                   "row", n + 2, "seq", n + 1, "occupied", false, "records", n,
                   "sheets", List.of("台账"));
    }

    static Map<String, Object> extractLinks(String body) {
        String text = str(field(body, "text"));
        Matcher m = Pattern.compile("(?:https?://)?mp\\.weixin\\.qq\\.com/[^\\s\"'<>，。；、）)]+").matcher(text);
        LinkedHashSet<String> set = new LinkedHashSet<>();
        while (m.find()) {
            String u = m.group();
            if (!u.startsWith("http")) u = "https://" + u;
            set.add(u);
        }
        return map("ok", true, "links", new ArrayList<>(set), "count", set.size());
    }

    static Map<String, Object> openFile() {
        try {
            if (Files.isRegularFile(ledgerCsv) && Desktop.isDesktopSupported())
                Desktop.getDesktop().open(ledgerCsv.toFile());
            return map("ok", true);
        } catch (Exception e) {
            return map("ok", false, "error", String.valueOf(e));
        }
    }

    // ---------------- 持久化 ----------------
    static void loadPool() {
        if (!Files.isRegularFile(poolJson)) return;
        try {
            String s = Files.readString(poolJson, StandardCharsets.UTF_8);
            Object o = Json.parse(s);
            if (o instanceof List) {
                for (Object x : (List<?>) o) if (x instanceof Map) pool.add((Map<String, Object>) x);
            }
        } catch (Exception ignored) {}
    }

    static void savePool() {
        try {
            Files.writeString(poolJson, Json.write(pool), StandardCharsets.UTF_8);
        } catch (Exception ignored) {}
    }

    /** 把 C++ 记录器抓到的链接并入链接池（只读导入，去重） */
    static void importCppLinks() {
        Path cpp = baseDir.resolve("cpp").resolve("data").resolve("wechat_links.jsonl");
        if (!Files.isRegularFile(cpp)) return;
        try {
            for (String line : Files.readAllLines(cpp, StandardCharsets.UTF_8)) {
                line = line.trim();
                if (line.isEmpty()) continue;
                String url = firstMatch(line, "\"url\"\\s*:\\s*\"([^\"]+)\"");
                if (url.isEmpty() || !url.contains("mp.weixin.qq.com")) continue;
                synchronized (LOCK) {
                    if (findPool(url) != null) continue;
                    String title = firstMatch(line, "\"title\"\\s*:\\s*\"([^\"]*)\"");
                    String time = firstMatch(line, "\"time\"\\s*:\\s*\"([^\"]*)\"");
                    Map<String, Object> it = new LinkedHashMap<>();
                    it.put("id", UUID.randomUUID().toString().substring(0, 10));
                    it.put("url", url);
                    it.put("title", title);
                    it.put("account", "");
                    it.put("date", "");
                    it.put("type", "");
                    it.put("theme", "");
                    it.put("writer", ""); it.put("layout", ""); it.put("photo", "");
                    it.put("review", ""); it.put("art", ""); it.put("secretary", "");
                    it.put("status", title.isEmpty() ? "partial" : "parsed");
                    it.put("source", "C++记录器");
                    it.put("created", time);
                    it.put("written", null);
                    pool.add(it);
                }
            }
            savePool();
        } catch (Exception ignored) {}
    }

    // ---------------- HTTP / JSON 工具 ----------------
    static String httpGet(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) URI.create(url).toURL().openConnection();
        c.setRequestProperty("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36");
        c.setRequestProperty("Accept-Encoding", "identity");
        c.setConnectTimeout(10000);
        c.setReadTimeout(15000);
        c.setInstanceFollowRedirects(true);
        try (InputStream in = c.getInputStream()) {
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n, total = 0;
            while ((n = in.read(buf)) > 0) {
                bos.write(buf, 0, n);
                total += n;
                if (total > 4_000_000) break;
            }
            return bos.toString(StandardCharsets.UTF_8);
        }
    }

    static String firstMatch(String html, String regex) {
        try {
            Matcher m = Pattern.compile(regex).matcher(html);
            if (m.find()) return m.group(1);
        } catch (Exception ignored) {}
        return "";
    }

    static String normalize(String url) {
        String u = url == null ? "" : url.trim();
        if (u.isEmpty()) return "";
        if (!u.startsWith("http")) u = "https://" + u;
        return u;
    }

    static String csvEscape(String s) {
        if (s == null) return "";
        if (s.contains(",") || s.contains("\"") || s.contains("\n") || s.contains("\r"))
            return "\"" + s.replace("\"", "\"\"") + "\"";
        return s;
    }

    static List<String> parseCsvLine(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder cur = new StringBuilder();
        boolean q = false;
        for (int i = 0; i < line.length(); i++) {
            char ch = line.charAt(i);
            if (q) {
                if (ch == '"') {
                    if (i + 1 < line.length() && line.charAt(i + 1) == '"') { cur.append('"'); i++; }
                    else q = false;
                } else cur.append(ch);
            } else {
                if (ch == '"') q = true;
                else if (ch == ',') { out.add(cur.toString()); cur.setLength(0); }
                else cur.append(ch);
            }
        }
        out.add(cur.toString());
        return out;
    }

    static String readBody(HttpExchange ex) throws IOException {
        try (InputStream in = ex.getRequestBody()) {
            return new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    static void sendJson(HttpExchange ex, Object o) throws IOException {
        byte[] b = Json.write(o).getBytes(StandardCharsets.UTF_8);
        send(ex, 200, "application/json; charset=utf-8", b);
    }

    static void send(HttpExchange ex, int code, String ct, byte[] b) throws IOException {
        ex.getResponseHeaders().set("Content-Type", ct);
        ex.getResponseHeaders().set("Cache-Control", "no-store");
        ex.sendResponseHeaders(code, b.length);
        try (OutputStream os = ex.getResponseBody()) { os.write(b); }
    }

    static Map<String, Object> map(Object... kv) {
        Map<String, Object> m = new LinkedHashMap<>();
        for (int i = 0; i + 1 < kv.length; i += 2) m.put(String.valueOf(kv[i]), kv[i + 1]);
        return m;
    }

    @SuppressWarnings("unchecked")
    static Object field(String body, String key) {
        try {
            Object o = Json.parse(body == null ? "{}" : body);
            if (o instanceof Map) return ((Map<String, Object>) o).get(key);
        } catch (Exception ignored) {}
        return null;
    }

    static String str(Object o) { return o == null ? "" : String.valueOf(o); }
}

/** 极简 JSON 读写（无第三方依赖） */
class Json {
    static String write(Object o) {
        StringBuilder sb = new StringBuilder();
        writeValue(sb, o);
        return sb.toString();
    }

    static void writeValue(StringBuilder sb, Object o) {
        if (o == null) { sb.append("null"); return; }
        if (o instanceof String) { writeString(sb, (String) o); return; }
        if (o instanceof Number || o instanceof Boolean) { sb.append(o); return; }
        if (o instanceof Map) {
            sb.append('{');
            boolean first = true;
            for (Map.Entry<?, ?> e : ((Map<?, ?>) o).entrySet()) {
                if (!first) sb.append(',');
                first = false;
                writeString(sb, String.valueOf(e.getKey()));
                sb.append(':');
                writeValue(sb, e.getValue());
            }
            sb.append('}');
            return;
        }
        if (o instanceof Iterable) {
            sb.append('[');
            boolean first = true;
            for (Object x : (Iterable<?>) o) {
                if (!first) sb.append(',');
                first = false;
                writeValue(sb, x);
            }
            sb.append(']');
            return;
        }
        writeString(sb, String.valueOf(o));
    }

    static void writeString(StringBuilder sb, String s) {
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default:
                    if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
                    else sb.append(c);
            }
        }
        sb.append('"');
    }

    static Object parse(String s) {
        P p = new P(s);
        Object v = p.value();
        p.ws();
        return v;
    }

    static class P {
        final String s; int i;
        P(String s) { this.s = s; }
        void ws() { while (i < s.length() && Character.isWhitespace(s.charAt(i))) i++; }
        Object value() {
            ws();
            if (i >= s.length()) return null;
            char c = s.charAt(i);
            if (c == '{') return obj();
            if (c == '[') return arr();
            if (c == '"') return strv();
            if (s.startsWith("true", i)) { i += 4; return true; }
            if (s.startsWith("false", i)) { i += 5; return false; }
            if (s.startsWith("null", i)) { i += 4; return null; }
            return num();
        }
        Map<String, Object> obj() {
            Map<String, Object> m = new LinkedHashMap<>();
            i++;
            ws();
            if (i < s.length() && s.charAt(i) == '}') { i++; return m; }
            while (i < s.length()) {
                ws();
                String k = strv();
                ws();
                if (i < s.length() && s.charAt(i) == ':') i++;
                m.put(k, value());
                ws();
                if (i < s.length() && s.charAt(i) == ',') { i++; continue; }
                if (i < s.length() && s.charAt(i) == '}') { i++; break; }
                break;
            }
            return m;
        }
        List<Object> arr() {
            List<Object> l = new ArrayList<>();
            i++;
            ws();
            if (i < s.length() && s.charAt(i) == ']') { i++; return l; }
            while (i < s.length()) {
                l.add(value());
                ws();
                if (i < s.length() && s.charAt(i) == ',') { i++; continue; }
                if (i < s.length() && s.charAt(i) == ']') { i++; break; }
                break;
            }
            return l;
        }
        String strv() {
            StringBuilder sb = new StringBuilder();
            i++;
            while (i < s.length()) {
                char c = s.charAt(i++);
                if (c == '"') break;
                if (c == '\\' && i < s.length()) {
                    char e = s.charAt(i++);
                    switch (e) {
                        case 'n': sb.append('\n'); break;
                        case 'r': sb.append('\r'); break;
                        case 't': sb.append('\t'); break;
                        case 'u':
                            if (i + 4 <= s.length()) { sb.append((char) Integer.parseInt(s.substring(i, i + 4), 16)); i += 4; }
                            break;
                        default: sb.append(e);
                    }
                } else sb.append(c);
            }
            return sb.toString();
        }
        Object num() {
            int st = i;
            while (i < s.length() && "-+.eE0123456789".indexOf(s.charAt(i)) >= 0) i++;
            String t = s.substring(st, i);
            try {
                if (t.contains(".") || t.contains("e") || t.contains("E")) return Double.parseDouble(t);
                return Long.parseLong(t);
            } catch (Exception e) { return 0L; }
        }
    }
}

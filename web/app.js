/* 推文台账 · 前端（单条录入 / 链接池 / 台账明细 / 悬浮模式） */
const S = {
  view: "live",
  liveTimer: null,
  liveItems: [],
  writtenUrls: new Set(),
  fields: [],
  options: { types: [], themes: [], accounts: [], people: [] },
  draft: {},
  sources: {},
  touched: false,
  sheets: [],
  counts: {},
  rowsData: {},
  activeSheet: null,
  ledgerSheet: null,
  mode: "append",
  insertRow: 0,
  workbook: "",
  plan: null,
  busy: false,
  pool: [],
  job: {},
  selected: new Set(),
  editing: null,
  pollTimer: null,
  importing: false,
};

const PLACEHOLDER = {
  seq: "写入时自动编号",
  type: "视频 / 贴图 / 推文",
  title: "与公众号原文标题一致，可改",
  url: "https://mp.weixin.qq.com/s/…",
  theme: "学院推文 / 一站式学生社区 / 学生组织建设",
  date: "2026年11月8日",
  writer: "供稿部门或姓名，没有就填 /",
  layout: "排版人姓名",
  account: "公众号名称，如 西建大环境学院",
  art: "封面设计人，没有就填 /",
  secretary: "秘书处整合、归档人，没有就填 /",
  photo: "摄影部门或姓名，没有就填 /",
  review: "审核人：辅导员 / 指导老师",
};

const DATALIST = {
  type: "dl-type", theme: "dl-theme", account: "dl-account",
  writer: "dl-people", layout: "dl-people", photo: "dl-people",
  art: "dl-people", review: "dl-people", secretary: "dl-people",
};

const $ = (id) => document.getElementById(id);

function esc(s) {
  return String(s == null ? "" : s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

async function api(path, body) {
  const opt = body === undefined
    ? { method: "GET" }
    : { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) };
  const res = await fetch(path, opt);
  const data = await res.json().catch(() => ({ ok: false, error: "服务器返回了无法解析的内容" }));
  if (data.ok === undefined) data.ok = res.ok;
  return data;
}

function toast(msg, isErr) {
  const t = $("toast");
  t.textContent = msg;
  t.className = "toast show" + (isErr ? " err" : "");
  clearTimeout(t._timer);
  t._timer = setTimeout(() => { t.className = "toast"; }, isErr ? 5200 : 2800);
}

function cnDate(v) {
  if (!v) return "";
  const m = String(v).match(/(\d{4})\D+(\d{1,2})\D+(\d{1,2})/);
  if (!m) return String(v);
  return m[1] + "年" + Number(m[2]) + "月" + Number(m[3]) + "日";
}

function yearMonth(dateStr) {
  const m = String(dateStr || "").match(/(\d{4})\D+(\d{1,2})/);
  return m ? m[1] + "年" + Number(m[2]) + "月" : "";
}

/* ================= 视图切换 ================= */

function setView(name) {
  S.view = name;
  ["live", "entry", "pool", "ledger"].forEach((v) => {
    $("view-" + v).classList.toggle("hidden", v !== name);
  });
  document.querySelectorAll(".view-btn").forEach((b) => {
    b.classList.toggle("on", b.dataset.view === name);
  });
  if (name === "live") startLive();
  else stopLive();
  if (name === "pool") loadPool();
  if (name === "ledger") loadRows();
}

/* ================= 动态识别 ================= */
function startLive() {
  stopLive();
  loadLive();
  S.liveTimer = setInterval(loadLive, 1000);   // 一秒刷新
}
function stopLive() {
  if (S.liveTimer) { clearInterval(S.liveTimer); S.liveTimer = null; }
}
async function loadLive() {
  try {
    const r = await api("/api/live");
    if (!r.ok) return;
    S.liveItems = r.items || [];
    renderLive(S.liveItems);
  } catch (e) { /* 忽略瞬时错误 */ }
}
function renderLive(items) {
  const box = $("live-rows");
  const badge = $("live-count");
  if (badge) {
    if (items.length) { badge.textContent = items.length; badge.classList.remove("hidden"); }
    else badge.classList.add("hidden");
  }
  $("live-sum").textContent = items.length
    ? "已识别 " + items.length + " 条 · 每 1 秒自动刷新（浏览器打开文章 / 复制链接都会出现）"
    : "等待识别… 浏览器里打开公众号文章、或在任何地方复制文章链接，1 秒内会出现在这里";
  box.innerHTML = "";
  if (!items.length) {
    const p = document.createElement("div");
    p.className = "pool-empty";
    p.textContent = "还没有识别到链接。";
    box.appendChild(p);
    return;
  }
  items.forEach((it) => {
    const row = document.createElement("div");
    row.className = "live-row";

    const t = document.createElement("span");
    t.className = "c-time";
    t.textContent = (it.time || "").slice(11) || it.time || "";

    const src = document.createElement("span");
    src.className = "c-src";
    const isClip = it.source === "剪贴板";
    const chip = document.createElement("span");
    chip.className = "chip " + (isClip ? "warn" : "done");
    chip.textContent = isClip ? "剪贴板" : "浏览器";
    src.appendChild(chip);

    const main = document.createElement("span");
    main.className = "c-main";
    const title = document.createElement("b");
    title.textContent = it.title || "（未读到标题）";
    const url = document.createElement("i");
    url.textContent = it.url;
    main.appendChild(title);
    main.appendChild(url);

    const act = document.createElement("span");
    act.className = "c-act";
    const btn = document.createElement("button");
    btn.className = "linkbtn";
    btn.type = "button";
    const done = S.writtenUrls.has(it.url);
    btn.textContent = done ? "已写入" : "一键写入";
    if (done) btn.style.color = "var(--muted)";
    btn.onclick = async () => {
      if (S.writtenUrls.has(it.url)) return;
      btn.disabled = true;
      btn.textContent = "写入中…";
      const r = await api("/api/quick-write", { url: it.url });
      if (r.ok) {
        S.writtenUrls.add(it.url);
        btn.disabled = false;
        btn.textContent = "已写入第" + r.row + "行";
        btn.style.color = "var(--muted)";
        toast("已写入「" + (r.title || it.url) + "」第 " + r.row + " 行");
      } else {
        btn.disabled = false;
        btn.textContent = "一键写入";
        toast(r.error || "写入失败", true);
      }
    };
    act.appendChild(btn);

    row.appendChild(t); row.appendChild(src); row.appendChild(main); row.appendChild(act);
    box.appendChild(row);
  });
}

/* ================= 启动 ================= */

async function boot() {
  const st = await api("/api/state");
  if (!st || st.error) {
    $("conn-text").textContent = "台账连接失败";
    $("conn-dot").className = "dot bad";
    return;
  }
  S.fields = st.fields || [];
  S.options = st.options || S.options;
  S.workbook = st.workbook || "";
  S.sheets = st.sheets || [];
  S.counts = st.records || {};
  renderDatalists();
  renderFields();
  renderTabs();
  if (S.sheets.length) {
    const ym = yearMonth(new Date().toISOString());
    S.activeSheet = S.sheets.includes(ym) ? ym : S.sheets[S.sheets.length - 1];
    S.ledgerSheet = S.activeSheet;
  }
  $("wb-name").textContent = S.workbook ? S.workbook.split(/[\\/]/).pop() : "还没有绑定台账";
  $("wb-name").title = S.workbook;
  if (st.note) {
    $("wb-note").textContent = st.note;
    $("wb-note").classList.remove("hidden");
  }
  if (st.exists) {
    $("conn-dot").className = "dot ok";
    const total = Object.values(S.counts).reduce((a, b) => a + b, 0);
    $("conn-text").textContent = "已连接 · " + S.sheets.length + " 张月度表 · " + total + " 条记录";
    await loadRows();
  } else {
    $("conn-dot").className = "dot bad";
    $("conn-text").textContent = "还没有绑定台账文件";
    $("wb-note").textContent = "点上面「换一个文件」选一份 xlsx 台账，选完就能写入。";
    $("wb-note").classList.remove("hidden");
  }
  refresh();
  applyMode();
  updatePoolBadge(st.poolPending || 0);
  startLive();
}

function renderDatalists() {
  const map = { "dl-type": S.options.types, "dl-theme": S.options.themes,
                "dl-account": S.options.accounts, "dl-people": S.options.people };
  Object.entries(map).forEach(([id, list]) => {
    const dl = $(id);
    dl.innerHTML = "";
    (list || []).forEach((v) => { const o = document.createElement("option"); o.value = v; dl.appendChild(o); });
  });
}

/* ================= 单条录入：表单 ================= */

function renderFields() {
  const box = $("fields");
  box.innerHTML = "";
  S.fields.forEach((f) => {
    const row = document.createElement("div");
    row.className = "field";
    row.dataset.key = f.key;

    const label = document.createElement("label");
    label.setAttribute("for", "f-" + f.key);
    label.textContent = f.head;
    if (f.required && f.key !== "seq") {
      const r = document.createElement("i");
      r.className = "req";
      label.appendChild(r);
    }

    const wrap = document.createElement("div");
    wrap.className = "inputwrap";
    const input = document.createElement("input");
    input.type = "text";
    input.id = "f-" + f.key;
    input.autocomplete = "off";
    input.spellcheck = false;
    input.placeholder = PLACEHOLDER[f.key] || "";
    if (f.key === "seq") input.readOnly = true;
    if (DATALIST[f.key]) input.setAttribute("list", DATALIST[f.key]);
    if (f.key === "url") input.style.fontFamily = "var(--ui-face)";
    input.addEventListener("input", () => {
      S.touched = true;
      S.draft[f.key] = input.value;
      S.sources[f.key] = input.value ? "manual" : "";
      paintField(f.key);
      refresh();
    });
    input.addEventListener("blur", () => {
      if (f.key === "date" && input.value) {
        const v = cnDate(input.value);
        if (v !== input.value) { input.value = v; S.draft.date = v; refresh(); }
      }
    });
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter" && f.key !== "title") { e.preventDefault(); input.blur(); }
    });

    const tag = document.createElement("span");
    tag.className = "tag none";
    tag.id = "tag-" + f.key;

    wrap.appendChild(input);
    wrap.appendChild(tag);
    row.appendChild(label);
    row.appendChild(wrap);
    box.appendChild(row);
  });
}

const TAG_TEXT = { auto: "自动", guess: "推测", manual: "手填", pick: "读取" };

function paintField(key) {
  const row = document.querySelector('.field[data-key="' + key + '"]');
  if (!row) return;
  const f = S.fields.find((x) => x.key === key);
  const val = (S.draft[key] || "").trim();
  const isMissing = !!f.required && !val && key !== "seq";
  row.classList.toggle("missing", isMissing);
  row.classList.toggle("warn", isMissing && S.touched);
  row.classList.toggle("filled", !!val);
  const tag = $("tag-" + key);
  const src = S.sources[key];
  if (tag) {
    if (src && val) { tag.className = "tag " + src; tag.textContent = TAG_TEXT[src] || "手填"; }
    else { tag.className = "tag none"; tag.textContent = ""; }
  }
}

function missingRequired() {
  return S.fields.filter((f) => f.required && f.key !== "seq" && !(S.draft[f.key] || "").trim());
}

function refresh() {
  S.fields.forEach((f) => paintField(f.key));
  const filled = S.fields.filter((f) => (S.draft[f.key] || "").trim()).length;
  $("fill-count").textContent = filled + " / " + S.fields.length + " 已填";
  const miss = missingRequired();
  const btn = $("btn-write");
  btn.disabled = miss.length > 0 || S.busy;
  btn.textContent = miss.length ? "还有 " + miss.length + " 项必填待补" : "写入台账";
  renderMissBar(miss);
  schedulePreview();
}

function renderMissBar(miss) {
  const bar = $("miss-bar");
  if (!miss.length) { bar.classList.add("hidden"); bar.innerHTML = ""; return; }
  bar.classList.remove("hidden");
  bar.innerHTML = "";
  const label = document.createElement("b");
  label.textContent = "必填待补 " + miss.length + " 项：";
  bar.appendChild(label);
  miss.forEach((f, i) => {
    if (i) bar.appendChild(document.createTextNode("、"));
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = f.head;
    b.onclick = () => { const el = $("f-" + f.key); if (el) { el.focus(); el.scrollIntoView({ block: "center", behavior: "smooth" }); } };
    bar.appendChild(b);
  });
}

/* ================= 单条录入：读取 ================= */

async function doParse() {
  const url = $("url").value.trim();
  if (!url) { toast("先粘贴公众号文章链接", true); $("url").focus(); return; }
  const btn = $("btn-parse");
  btn.disabled = true;
  btn.innerHTML = '<span class="spin"></span>读取中';
  $("parse-status").innerHTML = "";
  try {
    const r = await api("/api/parse", { url });
    if (!r.ok) {
      $("parse-status").innerHTML = '<p class="ps-warn">' + esc(r.error || "读取失败") +
        '</p><p class="hint">可以继续手动填写 13 个字段，照样能写进表里。</p>';
      setVal("url", url, "pick");
      toast("链接没读到，转手动填写", true);
      return;
    }
    applyParse(r);
  } catch (e) {
    toast("读取失败：" + e, true);
  } finally {
    btn.disabled = false;
    btn.textContent = "读取";
    refresh();
  }
}

function setVal(key, value, source) {
  if (value === undefined || value === null) return;
  const v = String(value);
  const input = $("f-" + key);
  if (!input) return;
  if (!v && (S.draft[key] || "").trim()) return;
  input.value = v;
  S.draft[key] = v;
  S.sources[key] = v ? (source || "manual") : "";
  paintField(key);
}

function applyParse(r) {
  setVal("url", r.url, "pick");
  setVal("title", r.title, "auto");
  setVal("account", r.account, "auto");
  setVal("date", r.date_cn || r.date, "auto");
  const sig = r.sigs || {};
  ["writer", "layout", "photo", "review", "art", "secretary"].forEach((k) => {
    if (sig[k]) setVal(k, sig[k], "auto");
  });
  setVal("type", r.type_guess, "guess");
  setVal("theme", r.theme_guess, "guess");

  const lines = [];
  if (r.title) lines.push(["推文名称", r.title]);
  if (r.account) lines.push(["公众号", r.account]);
  if (r.date_cn || r.date) lines.push(["发布时间", r.date_cn || r.date]);
  const found = Object.keys(sig).filter((k) => sig[k])
    .map((k) => (S.fields.find((f) => f.key === k) || {}).head + "：" + sig[k]);
  if (found.length) lines.push(["文末署名", found.join(" · ")]);

  let html = "";
  if (lines.length) {
    html += '<div class="ps-warn ps-ok"><p style="font-weight:600;margin-bottom:2px">已从页面读出 ' + lines.length + ' 组信息</p>';
    lines.forEach(([k, v]) => { html += "<p>" + esc(k) + "：" + esc(v) + "</p>"; });
    html += "</div>";
  }
  (r.warnings || []).forEach((w) => { html += '<p class="ps-warn">' + esc(w) + "</p>"; });
  const missNow = missingRequired();
  if (missNow.length) {
    html += '<p class="ps-warn">还缺 ' + missNow.length + ' 项必填：' +
      esc(missNow.map((f) => f.head).join("、")) + "。填好才能写入。</p>";
  }
  $("parse-status").innerHTML = html;
  S.touched = true;
  toast("已读取：" + (r.title || r.url));
}

/* ================= 单条录入：写入 ================= */

let previewTimer = null;
function schedulePreview() {
  clearTimeout(previewTimer);
  previewTimer = setTimeout(updatePreview, 320);
}

async function updatePreview() {
  const date = (S.draft.date || "").trim();
  if (!date) {
    $("plan-sheet").textContent = "—（先填发布时间）";
    $("plan-row").textContent = "—";
    $("plan-seq").textContent = "—";
    $("plan-warn").classList.add("hidden");
    return;
  }
  const p = await api("/api/preview", { date });
  if (!p.ok) { $("plan-sheet").textContent = p.error || "无法计算"; return; }
  S.plan = p;
  $("plan-sheet").textContent = p.sheet || "（按发布时间自动判定）";
  if (S.mode === "row") {
    $("plan-row").textContent = S.insertRow ? "第 " + S.insertRow + " 行（插入，后续行下移）" : "请填行号";
  } else {
    $("plan-row").textContent = "第 " + p.row + " 行（追加）";
  }
  $("plan-seq").textContent = "第 " + p.seq + " 条";
  $("plan-warn").classList.add("hidden");
}

function applyMode() { $("mode").value = S.mode; }

async function doWrite() {
  if (S.busy) return;
  const miss = missingRequired();
  if (miss.length) {
    toast("还缺：" + miss.map((f) => f.head).join("、"), true);
    const first = document.querySelector(".field.missing input");
    if (first) first.focus();
    return;
  }
  S.busy = true;
  const btn = $("btn-write");
  btn.disabled = true;
  btn.innerHTML = '<span class="spin"></span>写入中';
  const values = {};
  S.fields.forEach((f) => { values[f.key] = (S.draft[f.key] || "").trim(); });
  values.date = cnDate(values.date);
  try {
    const r = await api("/api/append", { values, mode: S.mode, row: S.mode === "row" ? S.insertRow : 0 });
    if (!r.ok) { toast(r.error || "写入失败", true); return; }
    toast("已写入「" + r.sheet + "」第 " + r.row + " 行（序号 " + r.seq + "）");
    resetForm();
    S.sheets = r.sheets || S.sheets;
    S.activeSheet = r.sheet;
    await loadRows();
  } catch (e) {
    toast("写入失败：" + e, true);
  } finally {
    S.busy = false;
    btn.textContent = "写入台账";
    refresh();
  }
}

function resetForm() {
  const keep = S.draft.account;
  S.fields.forEach((f) => {
    S.draft[f.key] = "";
    S.sources[f.key] = "";
    const input = $("f-" + f.key);
    if (input) input.value = "";
  });
  $("url").value = "";
  $("parse-status").innerHTML = "";
  S.touched = false;
  if (keep) setVal("account", keep, "manual");
  S.fields.forEach((f) => paintField(f.key));
}

/* ================= 台账数据 ================= */

async function loadRows() {
  const r = await api("/api/rows");
  if (!r || !r.rows) return;
  S.rowsData = r.rows;
  S.counts = {};
  Object.keys(r.rows).forEach((k) => { S.counts[k] = r.rows[k].length; });
  S.sheets = r.sheets || S.sheets;
  if (!S.activeSheet || !S.sheets.includes(S.activeSheet)) {
    S.activeSheet = S.sheets[S.sheets.length - 1] || null;
  }
  if (!S.ledgerSheet || !S.sheets.includes(S.ledgerSheet)) {
    S.ledgerSheet = S.activeSheet;
  }
  renderTabs();
  renderRecords();
  renderLedger();
}

function renderTabs() {
  const box = $("sheet-tabs");
  box.innerHTML = "";
  S.sheets.forEach((name) => {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "tab" + (name === S.activeSheet ? " on" : "");
    b.textContent = name;
    b.onclick = () => { S.activeSheet = name; renderTabs(); renderRecords(); };
    box.appendChild(b);
  });
}

function renderRecords() {
  const box = $("records");
  box.innerHTML = "";
  const rows = (S.rowsData && S.rowsData[S.activeSheet]) || [];
  if (!rows.length) {
    const li = document.createElement("li");
    li.className = "empty";
    li.textContent = "这张表还没有记录。";
    box.appendChild(li);
    return;
  }
  rows.slice().reverse().forEach((row, i) => {
    const li = document.createElement("li");
    const n = document.createElement("span");
    n.className = "n";
    n.textContent = row.seq || (rows.length - i);
    const t = document.createElement("span");
    t.className = "t";
    t.textContent = row.title || "（无标题）";
    t.title = row.title || "";
    const d = document.createElement("span");
    d.className = "d";
    d.textContent = row.date || "";
    li.appendChild(n); li.appendChild(t); li.appendChild(d);
    box.appendChild(li);
  });
}

function renderLedger() {
  const tabs = $("ledger-tabs");
  tabs.innerHTML = "";
  S.sheets.forEach((name) => {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "tab" + (name === S.ledgerSheet ? " on" : "");
    b.textContent = name + "（" + (S.counts[name] || 0) + "）";
    b.onclick = () => { S.ledgerSheet = name; renderLedger(); };
    tabs.appendChild(b);
  });
  const body = $("ledger-body");
  body.innerHTML = "";
  const rows = (S.rowsData && S.rowsData[S.ledgerSheet]) || [];
  if (!rows.length) {
    const tr = document.createElement("tr");
    tr.innerHTML = '<td colspan="13" style="padding:26px;color:var(--muted);font-family:var(--ui-face);font-size:13px">这张表还没有记录。</td>';
    body.appendChild(tr);
    return;
  }
  rows.forEach((row) => {
    const tr = document.createElement("tr");
    const cells = [row.seq, row.type, row.title, row.url, row.theme, row.date,
                   row.writer, row.layout, row.account, row.art, row.secretary, row.photo, row.review];
    cells.forEach((v, i) => {
      const td = document.createElement("td");
      if (i === 2) td.className = "t";
      if (i === 3) td.className = "u";
      td.textContent = v || "";
      if (i === 3 && v) { td.title = v; }
      tr.appendChild(td);
    });
    body.appendChild(tr);
  });
}

/* ================= 链接池 ================= */

function updatePoolBadge(n) {
  const b = $("pool-count");
  if (n > 0) { b.textContent = n; b.classList.remove("hidden"); }
  else { b.classList.add("hidden"); }
}

async function loadPool() {
  const r = await api("/api/pool");
  if (!r.ok) return;
  S.pool = r.items || [];
  S.job = r.job || {};
  updatePoolBadge(r.pending || 0);
  renderPool();
  renderProgress();
  clearTimeout(S.pollTimer);
  if (S.job.running) {
    S.pollTimer = setTimeout(loadPool, 1200);
  } else if (S.view === "pool") {
    S.pollTimer = setTimeout(loadPool, 3000);   // 池页面自动刷新，接入 C++ 记录器的新链接
  }
}

function renderProgress() {
  const wrap = $("progress");
  const job = S.job || {};
  if (!job.running && !job.total) { wrap.classList.add("hidden"); return; }
  wrap.classList.remove("hidden");
  const pct = job.total ? Math.round((job.done / job.total) * 100) : 0;
  $("bar-fill").style.width = pct + "%";
  $("progress-text").textContent = job.running
    ? "正在抓取 " + job.done + " / " + job.total + "（成功 " + (job.added || 0) + "，失败 " + (job.failed || 0) + "）"
    : "这一批抓完了：成功 " + (job.added || 0) + " 条，失败 " + (job.failed || 0) + " 条";
}

function statusChip(it) {
  if (it.written) return { cls: "done", text: "已写入 " + (it.written.sheet || "") };
  if (it.status === "error") return { cls: "err", text: "抓取失败" };
  if (it.missing && it.missing.length) return { cls: "warn", text: "缺 " + it.missing.length + " 项" };
  if (it.status === "partial") return { cls: "warn", text: "资料不全" };
  return { cls: "wait", text: "待写入" };
}

function renderPool() {
  const box = $("pool-rows");
  box.innerHTML = "";
  $("pool-sum").textContent = S.pool.length
    ? "共 " + S.pool.length + " 条 · 待写入 " + S.pool.filter((i) => !i.written).length + " · 已写入 " + S.pool.filter((i) => i.written).length
    : "空的。上面粘链接，或用悬浮按钮抓。";
  if (!S.pool.length) {
    const p = document.createElement("div");
    p.className = "pool-empty";
    p.textContent = "还没有条目。";
    box.appendChild(p);
    return;
  }
  S.pool.forEach((it) => {
    const row = document.createElement("div");
    row.className = "pool-row" + (it.written ? " done" : "");

    const chk = document.createElement("span");
    chk.className = "c-chk";
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = S.selected.has(it.id);
    cb.onchange = () => { cb.checked ? S.selected.add(it.id) : S.selected.delete(it.id); };
    chk.appendChild(cb);

    const title = document.createElement("span");
    title.className = "c-title";
    title.textContent = it.title || "（没读到标题，点开手填）";
    title.title = it.title || it.url;
    title.onclick = () => { S.editing = S.editing === it.id ? null : it.id; renderPool(); };

    const acc = document.createElement("span");
    acc.className = "c-acc";
    acc.textContent = it.account || "—";
    acc.title = it.account || "";

    const date = document.createElement("span");
    date.className = "c-date";
    date.textContent = it.date || "—";

    const tag = document.createElement("span");
    tag.className = "c-tag";
    tag.textContent = (it.type || "—") + " / " + (it.theme || "—");

    const state = document.createElement("span");
    state.className = "c-state";
    const chip = statusChip(it);
    const span = document.createElement("span");
    span.className = "chip " + chip.cls;
    span.textContent = chip.text;
    state.appendChild(span);

    row.appendChild(chk); row.appendChild(title); row.appendChild(acc);
    row.appendChild(date); row.appendChild(tag); row.appendChild(state);
    box.appendChild(row);

    if (S.editing === it.id) box.appendChild(buildEditor(it));
  });
}

function buildEditor(it) {
  const wrap = document.createElement("div");
  wrap.className = "editor";
  const defs = [
    ["title", "推文名称", true], ["account", "运营公众号", false], ["date", "发布时间", false],
    ["type", "类型", false], ["theme", "推文主题", false], ["layout", "运营排版", false],
    ["writer", "文案撰写", false], ["photo", "摄影", false], ["review", "审核", false],
    ["art", "美工封面", false], ["secretary", "秘书处整合、归档", false],
  ];
  defs.forEach(([key, label, wide]) => {
    const f = document.createElement("div");
    f.className = "ef" + (wide ? " wide" : "");
    const lb = document.createElement("label");
    lb.textContent = label;
    const inp = document.createElement("input");
    inp.value = it[key] || "";
    inp.placeholder = PLACEHOLDER[key] || "";
    if (DATALIST[key]) inp.setAttribute("list", DATALIST[key]);
    inp.className = inp.value ? "" : "empty";
    const commit = async () => {
      const v = inp.value.trim();
      if (v === (it[key] || "")) return;
      it[key] = v;
      inp.className = v ? "" : "empty";
      await api("/api/pool/update", { id: it.id, patch: { [key]: v } });
      const item = S.pool.find((x) => x.id === it.id);
      if (item) { item[key] = v; item.missing = recomputeMissing(item); }
      updatePoolBadge(S.pool.filter((x) => !x.written).length);
    };
    inp.onblur = commit;
    inp.onkeydown = (e) => { if (e.key === "Enter") inp.blur(); };
    f.appendChild(lb); f.appendChild(inp);
    wrap.appendChild(f);
  });

  const cmd = document.createElement("div");
  cmd.className = "ef wide ecmd";
  const bWrite = document.createElement("button");
  bWrite.className = "btn btn-seal";
  bWrite.type = "button";
  bWrite.textContent = "写入这一条";
  bWrite.onclick = async () => { await writePool([it.id]); };
  const bOpen = document.createElement("button");
  bOpen.className = "linkbtn";
  bOpen.type = "button";
  bOpen.textContent = "打开原文";
  bOpen.onclick = () => window.open(it.url, "_blank");
  const bDel = document.createElement("button");
  bDel.className = "linkbtn";
  bDel.type = "button";
  bDel.textContent = "删除";
  bDel.onclick = async () => {
    await api("/api/pool/remove", { ids: [it.id] });
    S.editing = null;
    loadPool();
  };
  cmd.appendChild(bWrite); cmd.appendChild(bOpen); cmd.appendChild(bDel);
  wrap.appendChild(cmd);
  return wrap;
}

function recomputeMissing(it) {
  const req = S.fields.filter((f) => f.required && f.key !== "seq");
  return req.filter((f) => !String(it[f.key] || "").trim()).map((f) => f.head);
}

/* ---------------- 导入 ---------------- */

async function fetchUrls(urls, source) {
  if (!urls.length) { toast("没识别到公众号链接", true); return; }
  const r = await api("/api/pool/fetch", { urls, source });
  if (!r.ok) { toast(r.error || "排队失败", true); return; }
  toast("已排队 " + r.queued + " 条，正在抓取…");
  setView("pool");
  loadPool();
}

async function importPaste() {
  const text = $("paste").value.trim();
  if (!text) { toast("先把链接粘进来", true); return; }
  const r = await api("/api/urls/extract", { text });
  if (!r.ok) { toast(r.error || "识别失败", true); return; }
  $("import-hint").textContent = "识别到 " + r.count + " 条链接";
  if (!r.count) { toast("这段文字里没有公众号链接", true); return; }
  $("paste").value = "";
  fetchUrls(r.links, "paste");
}

async function importFile(file) {
  const btn = $("btn-file");
  btn.disabled = true;
  btn.textContent = "读取中…";
  try {
    const buf = await file.arrayBuffer();
    const res = await fetch("/api/urls/upload", {
      method: "POST",
      headers: { "X-Filename": encodeURIComponent(file.name), "Content-Type": "application/octet-stream" },
      body: buf,
    });
    const r = await res.json();
    if (!r.ok) { toast(r.error || "读取失败", true); return; }
    $("import-hint").textContent = file.name + "：识别到 " + r.count + " 条链接";
    if (!r.count) { toast("这个文件里没有公众号链接", true); return; }
    fetchUrls(r.links, "file");
  } catch (e) {
    toast("读取失败：" + e, true);
  } finally {
    btn.disabled = false;
    btn.textContent = "导入表格 / 文本";
  }
}

/* ---------------- 写入 ---------------- */

async function writePool(ids) {
  const list = ids || S.pool.filter((i) => !i.written).map((i) => i.id);
  if (!list.length) { toast("没有待写入的条目", true); return; }
  const r = await api("/api/pool/write", {
    ids: list,
    fillSlash: $("fill-slash").checked,
    mode: S.mode,
    row: S.mode === "row" ? S.insertRow : 0,
  });
  if (!r.ok) {
    toast(r.error || "写入失败", true);
    loadPool();
    return;
  }
  const ok = r.written || 0;
  const skip = typeof r.skipped === "number" ? r.skipped : (r.skipped || []).length;
  toast("写入 " + ok + " 条" + (skip ? "，跳过 " + skip + " 条（缺发布时间或标题）" : ""));
  S.selected.clear();
  await loadPool();
  await loadRows();
}

/* ================= 悬浮模式 ================= */

async function toggleFloating() {
  const r = await api("/api/floating/start", {});
  if (!r.ok) { toast(r.error || "启动悬浮按钮失败", true); return; }
  toast("悬浮按钮已出现在桌面右上角；鼠标移上去有「抓取链接 / 退出」");
  $("btn-float").textContent = "悬浮按钮运行中";
}

/* ================= 事件 ================= */

function bind() {
  document.querySelectorAll(".view-btn").forEach((b) => {
    b.onclick = () => setView(b.dataset.view);
  });

  $("btn-parse").addEventListener("click", doParse);
  $("url").addEventListener("keydown", (e) => { if (e.key === "Enter") { e.preventDefault(); doParse(); } });
  $("btn-write").addEventListener("click", doWrite);
  $("mode").addEventListener("change", (e) => {
    S.mode = e.target.value;
    const rowInput = $("insert-row");
    rowInput.classList.toggle("hidden-line", S.mode !== "row");
    if (S.mode === "row" && !rowInput.value) rowInput.value = "4";
    updatePreview();
  });
  $("insert-row").addEventListener("input", (e) => {
    S.insertRow = parseInt(e.target.value, 10) || 0;
    updatePreview();
  });
  $("btn-slash").addEventListener("click", () => {
    S.fields.forEach((f) => {
      if (f.required || f.key === "seq") return;
      if (!(S.draft[f.key] || "").trim()) setVal(f.key, "/", "manual");
    });
    refresh();
    toast("未填的非必填项已填「/」");
  });
  $("btn-undo").addEventListener("click", async () => {
    if (!confirm("还原到上一次写入之前的状态？")) return;
    const r = await api("/api/undo", {});
    if (r.ok) { toast("已还原：" + r.restored); await loadRows(); }
    else toast(r.error || "还原失败", true);
  });
  $("btn-pick").addEventListener("click", async () => {
    toast("正在打开文件选择框…");
    const r = await api("/api/pick-file", {});
    if (r.ok) { toast("已切换台账"); await boot(); }
    else if (!r.cancelled) toast(r.error || "选择失败", true);
  });
  $("btn-open").addEventListener("click", async () => {
    const r = await api("/api/open-file", {});
    if (!r.ok) toast(r.error || "打不开文件", true);
  });
  $("btn-float").addEventListener("click", () => toast("悬浮窗已取消，请用「动态识别」页"));
  $("btn-live-refresh").addEventListener("click", loadLive);
  $("btn-live-clear").addEventListener("click", () => {
    S.liveItems = [];
    renderLive([]);
    toast("已清空显示（内核会记住已识别的链接，不重复弹出）");
  });

  $("btn-grab").addEventListener("click", importPaste);
  $("paste").addEventListener("keydown", (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === "Enter") { e.preventDefault(); importPaste(); }
  });
  $("btn-file").addEventListener("click", () => $("file").click());
  $("file").addEventListener("change", (e) => {
    const f = e.target.files && e.target.files[0];
    if (f) importFile(f);
    e.target.value = "";
  });
  $("chk-all").addEventListener("change", (e) => {
    S.selected.clear();
    if (e.target.checked) S.pool.filter((i) => !i.written).forEach((i) => S.selected.add(i.id));
    renderPool();
  });
  $("btn-write-all").addEventListener("click", () => writePool(null));
  $("btn-write-sel").addEventListener("click", () => writePool([...S.selected]));
  $("btn-remove-sel").addEventListener("click", async () => {
    const ids = [...S.selected];
    if (!ids.length) { toast("先勾选要删的", true); return; }
    await api("/api/pool/remove", { ids });
    S.selected.clear();
    loadPool();
  });
  $("btn-clear").addEventListener("click", async () => {
    await api("/api/pool/clear", { all: false });
    loadPool();
  });

  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && S.editing) { S.editing = null; renderPool(); }
  });
}

bind();
boot();

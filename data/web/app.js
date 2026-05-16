// Dash portal — vanilla JS controller. Mobile Safari / Chrome target.
//
// One file because the portal is small and loading a second JS file off
// LittleFS isn't worth the round-trip. Sections marked with header comments.

const $  = (id) => document.getElementById(id);
const $$ = (sel) => document.querySelectorAll(sel);

/* =========================================================================
 * State
 * ========================================================================= */

const state = {
  status: null,
  config: null,
  session: null,
  stats: null,
  selectedMinutes: 25,
  activeTab: "study",
  lastError: null,
};

const MOOD_NAMES = ["neutral", "focused", "excited", "tired", "listening", "playful"];

/* =========================================================================
 * Networking
 * ========================================================================= */

async function api(path, opts = {}) {
  const ctrl = new AbortController();
  const timeout = setTimeout(() => ctrl.abort(), 8000);
  try {
    const resp = await fetch(path, {
      headers: { "content-type": "application/json", ...(opts.headers || {}) },
      signal: ctrl.signal,
      ...opts,
    });
    if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
    const ct = resp.headers.get("content-type") || "";
    return ct.includes("json") ? await resp.json() : await resp.text();
  } finally {
    clearTimeout(timeout);
  }
}

/* =========================================================================
 * Toast
 * ========================================================================= */

let toastTimer = null;
function toast(msg, kind = "") {
  const t = $("toast");
  t.textContent = msg;
  t.classList.remove("err");
  if (kind === "err") t.classList.add("err");
  t.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove("show"), 2400);
}

/* =========================================================================
 * Status pill
 * ========================================================================= */

function setPill(cls, text) {
  const p = $("status-pill");
  p.classList.remove("ok", "err", "busy");
  if (cls) p.classList.add(cls);
  p.textContent = text;
}

/* =========================================================================
 * Time sync (one-shot at boot)
 * ========================================================================= */

async function timeSync() {
  try {
    await api("/api/time-sync", {
      method: "POST",
      body: JSON.stringify({
        unix_ms: Date.now(),
        tz_min: -new Date().getTimezoneOffset(),
      }),
    });
  } catch (e) { /* not fatal */ }
}

/* =========================================================================
 * Status / config / session / stats
 * ========================================================================= */

async function refreshStatus() {
  try {
    state.status = await api("/api/status");
    setPill("ok", state.status.state);
    $("device-name").textContent = state.status.name || "Dash";
    const m = state.status.mood;
    $("mood-text").textContent =
      describeStatusLine(state.status);
  } catch (e) {
    setPill("err", "offline");
    state.lastError = String(e);
  }
}

function describeStatusLine(s) {
  if (!s) return "—";
  switch (s.state) {
    case "Onboarding": return "ready to meet you";
    case "Idle":       return "ready when you are";
    case "Drowsy":     return "getting sleepy";
    case "Asleep":     return "asleep — touch to wake";
    case "InSession":  return "focused with you";
    case "InMenu":     return "in the menu";
    case "InGame":     return "playing";
    case "OtaChecking":return "looking for updates";
    case "GroupSessionActive":  return "in a group session";
    case "GroupSessionWaiting": return "waiting for friends";
    default:           return s.state.toLowerCase();
  }
}

async function refreshConfig() {
  try {
    state.config = await api("/api/config");
    const c = state.config;
    $("cfg-name").value = c.name || "";
    $("cfg-volume").value = c.volume ?? 60;
    $("vol-label").textContent = c.volume ?? 60;
    $("cfg-sleep-min").value = Math.round((c.sleep_timeout_s || 180) / 60);
    $("cfg-session-min").value = c.session_minutes || 25;
    // Sync the selected chip default.
    selectMinutes(c.session_minutes || 25);
  } catch (e) {}
}

async function refreshHomeWifi() {
  try {
    const ob = await api("/api/onboarding");
    // Note: API doesn't return ssid string for security; we just show whether set.
    if (ob.home_wifi_set) {
      $("cfg-home-ssid").placeholder = "(saved)";
    }
  } catch (e) {}
}

async function refreshSession() {
  try {
    state.session = await api("/api/session");
    renderSession();
  } catch (e) {}
}

async function refreshStats() {
  try {
    state.stats = await api("/api/stats");
    renderStats();
  } catch (e) {}
}

/* =========================================================================
 * Session render
 * ========================================================================= */

function renderSession() {
  const s = state.session;
  if (!s || !s.active) {
    $("session-idle").hidden = false;
    $("session-running").hidden = true;
    return;
  }
  $("session-idle").hidden = true;
  $("session-running").hidden = false;
  const elapsed = s.elapsed_s | 0;
  const total = s.total_s | 0;
  const label = (s.label || "").trim();
  $("running-label").textContent = label || "In session";
  $("timer").textContent = formatTimer(elapsed);
  $("timer-sub").textContent =
    `of ${formatTimer(total)} · ${s.distractions} distraction${s.distractions === 1 ? "" : "s"}`;
  const pct = total > 0 ? Math.min(100, (elapsed / total) * 100) : 0;
  $("progress-fill").style.width = `${pct}%`;
  const paused = s.state === 2;
  $("running-mood").textContent = paused
    ? "paused"
    : (s.distractions === 0 ? "in the zone" : "stay with it");
  const pauseBtn = $("btn-pause-session");
  if (pauseBtn) pauseBtn.textContent = paused ? "Resume" : "Pause";
}

function formatTimer(s) {
  const m = Math.floor(s / 60);
  const sec = Math.floor(s % 60);
  return `${m.toString().padStart(2, "0")}:${sec.toString().padStart(2, "0")}`;
}

/* =========================================================================
 * Stats render
 * ========================================================================= */

function renderStats() {
  const s = state.stats;
  if (!s) return;

  const focusedMin = Math.round((s.total_focused_sec || 0) / 60);
  const totalH = (focusedMin / 60).toFixed(1);
  if ((s.total_sessions || 0) === 0) {
    $("stats-total").innerHTML =
      `<span style="font-size:var(--fs-xl);font-weight:600;color:var(--fg-dim)">Nothing yet</span>` +
      `<div class="subtle" style="margin-top:var(--s-2)">Your focus stats will live here once you finish your first session.</div>`;
  } else {
    $("stats-total").innerHTML =
      `<span>${totalH}</span><span class="unit">h focused · ${s.total_sessions} session${s.total_sessions === 1 ? "" : "s"}</span>`;
  }
  $("stats-streak").textContent = s.streak_days
    ? `${s.streak_days} day${s.streak_days === 1 ? "" : "s"} 🔥`
    : "—";
  const bestMin = Math.round((s.best_single_sec || 0) / 60);
  $("stats-best").textContent = bestMin ? `${bestMin} min` : "—";
  $("stats-distractions").textContent = s.total_distractions || 0;

  // 7-day bars: bucket recent sessions by day relative to "today".
  renderWeekBars(s.recent || []);

  // History list.
  const hist = $("history-list");
  hist.innerHTML = "";
  if (!s.recent || s.recent.length === 0) {
    hist.innerHTML = '<div class="subtle">No sessions yet — head to the Study tab and start one. 🌱</div>';
    return;
  }
  for (const r of s.recent.slice().reverse()) {
    const date = r.u ? new Date(r.u * 1000) : null;
    const when = date ? friendlyTime(date) : "—";
    const min = Math.round((r.as || 0) / 60);
    const target = r.tm || 0;
    const completed = r.c === 1;
    const node = document.createElement("div");
    node.className = "row";
    node.innerHTML =
      `<div class="col">
         <span style="font-weight:600">${min} min focus</span>
         <span class="meta">${when} · target ${target} min · ${r.d || 0} distractions</span>
       </div>
       <span class="${completed ? 'text-success' : 'muted'}">${completed ? '✓' : '·'}</span>`;
    hist.appendChild(node);
  }
}

function friendlyTime(d) {
  const now = new Date();
  const sameDay = d.toDateString() === now.toDateString();
  if (sameDay) {
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  }
  const diffDays = Math.floor((now - d) / 86400000);
  if (diffDays < 7) return `${diffDays}d ago`;
  return d.toLocaleDateString();
}

function renderWeekBars(recent) {
  const wb = $("week-bars");
  wb.innerHTML = "";
  const now = new Date();
  const days = []; // index 0 = 6 days ago, index 6 = today
  for (let i = 0; i < 7; i++) {
    days.push({ minutes: 0, label: ["S","M","T","W","T","F","S"][new Date(now.getTime() - (6 - i) * 86400000).getDay()] });
  }
  let max = 0;
  for (const r of recent || []) {
    if (!r.u) continue;
    const d = new Date(r.u * 1000);
    const diffDays = Math.floor((now - d) / 86400000);
    const idx = 6 - diffDays;
    if (idx >= 0 && idx < 7) {
      days[idx].minutes += Math.round((r.as || 0) / 60);
      if (days[idx].minutes > max) max = days[idx].minutes;
    }
  }
  for (const day of days) {
    const bar = document.createElement("div");
    const h = max > 0 ? Math.max(2, (day.minutes / max) * 80) : 2;
    bar.style.height = `${h}px`;
    if (day.minutes === 0) bar.style.opacity = "0.3";
    bar.innerHTML = `<span class="label">${day.label}</span>`;
    wb.appendChild(bar);
  }
  const todayMin = days[6].minutes;
  $("week-summary").textContent = todayMin > 0
    ? `${todayMin} min focused today`
    : (max > 0 ? "no focus yet today" : "a quiet week");
}

/* =========================================================================
 * Duration chips
 * ========================================================================= */

function selectMinutes(min) {
  state.selectedMinutes = min;
  $$("#duration-chips .chip").forEach(c => {
    const v = c.dataset.min;
    const isPick = (v == "custom" && min === "custom") ||
                   (v == String(min)) ||
                   (v == "custom" && typeof min === "number" && ![25,45,60,90].includes(min));
    c.setAttribute("aria-pressed", isPick);
  });
  const custom = $("custom-min");
  if (min === "custom" || (typeof min === "number" && ![25,45,60,90].includes(min))) {
    custom.hidden = false;
    if (typeof min === "number") custom.value = min;
    custom.focus();
  } else {
    custom.hidden = true;
  }
}

function currentMinutes() {
  if (state.selectedMinutes === "custom") {
    const v = parseInt($("custom-min").value || "25", 10);
    return Math.max(5, Math.min(240, v));
  }
  return typeof state.selectedMinutes === "number" ? state.selectedMinutes : 25;
}

/* =========================================================================
 * Actions
 * ========================================================================= */

async function startSession() {
  const minutes = currentMinutes();
  const label = $("session-label").value;
  $("btn-start-session").disabled = true;
  try {
    await api("/api/session", {
      method: "POST",
      body: JSON.stringify({ action: "start", minutes, label }),
    });
    toast(`session started — ${minutes} min`);
    await refreshSession();
  } catch (e) {
    toast("could not start session", "err");
  } finally {
    $("btn-start-session").disabled = false;
  }
}

async function endSession() {
  if (!confirm("End the session early?")) return;
  try {
    await api("/api/session", { method: "POST", body: JSON.stringify({ action: "stop" }) });
    toast("session ended");
    await refreshSession();
    await refreshStats();
  } catch (e) { toast("end failed", "err"); }
}

async function pauseOrResumeSession() {
  if (!state.session) return;
  const isPaused = state.session.state === 2; // SessionState::Paused
  try {
    await api("/api/session", {
      method: "POST",
      body: JSON.stringify({ action: isPaused ? "resume" : "pause" }),
    });
    toast(isPaused ? "resumed" : "paused");
    await refreshSession();
  } catch (e) { toast("pause failed", "err"); }
}

async function saveConfig() {
  const body = {
    name: $("cfg-name").value || "Dash",
    volume: Number($("cfg-volume").value),
    sleep_timeout_s: Number($("cfg-sleep-min").value || 3) * 60,
    session_minutes: Number($("cfg-session-min").value || 25),
    home_ssid: $("cfg-home-ssid").value || undefined,
    home_password: $("cfg-home-pw").value || undefined,
  };
  try {
    await api("/api/config", { method: "POST", body: JSON.stringify(body) });
    toast("saved");
    refreshStatus();
  } catch (e) { toast("save failed", "err"); }
}

async function replayOnboarding() {
  if (!confirm("Replay the welcome tutorial?")) return;
  try {
    await api("/api/onboarding", { method: "POST", body: JSON.stringify({ reset: true }) });
    location.href = "/onboarding.html";
  } catch (e) { toast("could not reset", "err"); }
}

async function otaCheck() {
  setPill("busy", "checking…");
  $("btn-ota-check").disabled = true;
  try {
    await api("/api/ota/check", { method: "POST" });
    toast("checking — Dash may reboot if there's an update");
  } catch (e) {
    toast("update check failed", "err");
    setPill("err", "ota error");
  } finally {
    $("btn-ota-check").disabled = false;
    setTimeout(refreshStatus, 1500);
  }
}

async function resetStats() {
  if (!confirm("Delete all session history? This can't be undone.")) return;
  try {
    await api("/api/stats", { method: "DELETE" });
    toast("stats reset");
    refreshStats();
  } catch (e) { toast("reset failed", "err"); }
}

async function factoryReset() {
  if (!confirm("Factory reset Dash? You'll need to onboard again.")) return;
  if (!confirm("Are you really sure? Settings and stats will all be cleared.")) return;
  try {
    await api("/api/factory-reset", { method: "POST" });
    toast("resetting…");
    setTimeout(() => location.href = "/onboarding.html", 1500);
  } catch (e) { toast("reset failed", "err"); }
}

async function startGroupStudy() {
  try {
    await api("/api/group", { method: "POST", body: JSON.stringify({ action: "start" }) });
    toast("looking for nearby Dashes");
  } catch (e) { toast("group study failed", "err"); }
}

/* =========================================================================
 * Tabs
 * ========================================================================= */

function switchTab(tab) {
  state.activeTab = tab;
  $$(".tab").forEach(t => t.setAttribute("aria-current", t.dataset.tab === tab));
  $$(".tab-page").forEach(p => p.classList.toggle("active", p.id === `tab-${tab}`));
  // Refresh data when tab becomes visible.
  if (tab === "stats") refreshStats();
}

/* =========================================================================
 * Bind
 * ========================================================================= */

function bind() {
  $("btn-start-session").addEventListener("click", startSession);
  $("btn-end-session").addEventListener("click", endSession);
  const pauseBtn = $("btn-pause-session");
  if (pauseBtn) pauseBtn.addEventListener("click", pauseOrResumeSession);
  $("btn-save-config").addEventListener("click", saveConfig);
  $("btn-replay-onboarding").addEventListener("click", replayOnboarding);
  $("btn-ota-check").addEventListener("click", otaCheck);
  $("btn-reset-stats").addEventListener("click", resetStats);
  $("btn-factory-reset").addEventListener("click", factoryReset);
  $("btn-group-study").addEventListener("click", startGroupStudy);

  // Volume slider: live label + debounced API call so user hears a test
  // tick at the new level. Throttled to one /api/config write per second to
  // avoid NVS thrash while the user drags the slider.
  let volTimer = null;
  $("cfg-volume").addEventListener("input", (e) => {
    $("vol-label").textContent = e.target.value;
    clearTimeout(volTimer);
    volTimer = setTimeout(() => {
      const v = Number(e.target.value);
      api("/api/config", {
        method: "POST",
        body: JSON.stringify({ volume: v }),
      }).then(() => api("/api/test-tone", { method: "POST" }))
        .catch(() => {});
    }, 700);
  });

  $("btn-toggle-pw").addEventListener("click", () => {
    const i = $("cfg-home-pw");
    if (i.type === "password") { i.type = "text"; $("btn-toggle-pw").textContent = "hide"; }
    else { i.type = "password"; $("btn-toggle-pw").textContent = "show"; }
  });

  $$("#duration-chips .chip").forEach(c => {
    c.addEventListener("click", () => {
      const v = c.dataset.min;
      selectMinutes(v === "custom" ? "custom" : parseInt(v, 10));
    });
  });

  $$(".tab").forEach(t => t.addEventListener("click", () => switchTab(t.dataset.tab)));

  // Theme picker.
  $$("#theme-chips .chip").forEach(c => {
    c.addEventListener("click", () => applyTheme(c.dataset.theme));
  });

  // Konami code easter egg.
  let buf = [];
  const code = ["ArrowUp","ArrowUp","ArrowDown","ArrowDown","ArrowLeft","ArrowRight","ArrowLeft","ArrowRight","b","a"];
  document.addEventListener("keydown", (e) => {
    buf.push(e.key);
    if (buf.length > code.length) buf.shift();
    if (buf.join(",") === code.join(",")) { konami(); buf = []; }
  });
}

async function konami() {
  toast("🎉 Dash sees you", "");
  try { await api("/api/easter-egg", { method: "POST" }); } catch (e) {}
}

/* =========================================================================
 * Theme (frontend-only, persisted in localStorage)
 * ========================================================================= */

function applyTheme(theme) {
  document.documentElement.setAttribute("data-theme", theme);
  localStorage.setItem("dash.theme", theme);
  $$("#theme-chips .chip").forEach(c =>
    c.setAttribute("aria-pressed", c.dataset.theme === theme));
}

function loadSavedTheme() {
  const t = localStorage.getItem("dash.theme") || "warm";
  applyTheme(t);
}

/* =========================================================================
 * Boot
 * ========================================================================= */

async function boot() {
  loadSavedTheme();
  bind();
  await timeSync();
  // Redirect first-boot users to the wizard.
  try {
    const ob = await api("/api/onboarding");
    if (ob && !ob.onboarded && location.pathname !== "/onboarding.html") {
      location.href = "/onboarding.html";
      return;
    }
  } catch (e) {}
  await Promise.all([refreshStatus(), refreshConfig(), refreshSession(), refreshHomeWifi()]);
  await refreshStats();
  setInterval(refreshStatus, 4000);
  setInterval(refreshSession, 1000);
  setInterval(refreshStats, 30000);
}

document.addEventListener("DOMContentLoaded", boot);

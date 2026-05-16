// Dash captive-portal client. Targets evergreen mobile Safari/Chrome —
// vanilla JS, no framework, no bundler.

const $ = (id) => document.getElementById(id);

const state = {
  status: null,
  config: null,
  session: null,
};

async function api(path, opts = {}) {
  const resp = await fetch(path, {
    headers: { "content-type": "application/json", ...(opts.headers || {}) },
    ...opts,
  });
  if (!resp.ok) throw new Error(`${path}: HTTP ${resp.status}`);
  const ct = resp.headers.get("content-type") || "";
  return ct.includes("json") ? await resp.json() : await resp.text();
}

async function timeSync() {
  try {
    await api("/api/time-sync", {
      method: "POST",
      body: JSON.stringify({
        unix_ms: Date.now(),
        tz_min: -new Date().getTimezoneOffset(),
      }),
    });
  } catch (e) {
    console.warn("time-sync failed", e);
  }
}

async function refreshStatus() {
  try {
    state.status = await api("/api/status");
    setPill("ok", state.status.state);
    $("device-name").textContent = state.status.name || "Dash";
    $("device-state").textContent = describeState(state.status);
    $("footer").textContent = `firmware ${state.status.firmware} · boot #${state.status.boot_count}`;
  } catch (e) {
    setPill("err", "offline");
  }
}

async function refreshConfig() {
  try {
    state.config = await api("/api/config");
    $("cfg-name").value = state.config.name || "";
    $("cfg-volume").value = state.config.volume ?? 60;
    $("cfg-sleep-min").value = Math.round((state.config.sleep_timeout_s || 180) / 60);
    $("cfg-session-min").value = state.config.session_minutes || 25;
  } catch (e) {
    console.warn("config fetch", e);
  }
}

async function refreshSession() {
  try {
    state.session = await api("/api/session");
    renderSession();
  } catch (e) { /* session API may not be wired until M6 */ }
}

async function refreshStats() {
  try {
    const s = await api("/api/stats");
    const total = s.total_sessions || 0;
    if (total === 0) {
      $("stats-summary").textContent = "no sessions yet";
      return;
    }
    const focusedMin = Math.round((s.total_focused_sec || 0) / 60);
    const best = Math.round((s.best_single_sec || 0) / 60);
    $("stats-summary").innerHTML =
      `<b>${s.completed_sessions}/${total}</b> completed · ` +
      `<b>${focusedMin}</b> min focused · ` +
      `<b>${s.total_distractions}</b> distractions · ` +
      `best <b>${best}</b> min`;
  } catch (e) {}
}

function describeState(s) {
  if (!s) return "—";
  return `${s.state} · ${s.face}`;
}

function renderSession() {
  if (!state.session || !state.session.active) {
    $("session-progress").hidden = true;
    $("btn-start-session").hidden = false;
    $("btn-end-session").hidden = true;
    return;
  }
  $("session-progress").hidden = false;
  $("btn-start-session").hidden = true;
  $("btn-end-session").hidden = false;
  const pct = Math.min(100, (state.session.elapsed_s / state.session.total_s) * 100);
  $("progress-fill").style.width = `${pct}%`;
  $("session-elapsed").textContent = formatTime(state.session.elapsed_s);
  $("session-total").textContent = formatTime(state.session.total_s);
}

function formatTime(s) {
  const m = Math.floor(s / 60);
  const sec = Math.floor(s % 60);
  return `${m}:${sec.toString().padStart(2, "0")}`;
}

function setPill(cls, text) {
  const pill = $("status-pill");
  pill.classList.remove("ok", "err");
  if (cls) pill.classList.add(cls);
  pill.textContent = text;
}

async function startSession() {
  const minutes = Number($("cfg-session-min").value) || 25;
  try {
    await api("/api/session", {
      method: "POST",
      body: JSON.stringify({ action: "start", minutes }),
    });
    await refreshSession();
  } catch (e) { alert("could not start session: " + e.message); }
}

async function endSession() {
  try {
    await api("/api/session", { method: "POST", body: JSON.stringify({ action: "stop" }) });
    await refreshSession();
  } catch (e) {}
}

async function saveConfig() {
  const body = {
    name: $("cfg-name").value,
    volume: Number($("cfg-volume").value),
    sleep_timeout_s: Number($("cfg-sleep-min").value) * 60,
    session_minutes: Number($("cfg-session-min").value),
  };
  try {
    await api("/api/config", { method: "POST", body: JSON.stringify(body) });
    setPill("ok", "saved");
    setTimeout(refreshStatus, 600);
  } catch (e) { setPill("err", "save failed"); }
}

async function replayOnboarding() {
  if (!confirm("Replay the welcome tutorial?")) return;
  try {
    await api("/api/onboarding", {
      method: "POST",
      body: JSON.stringify({ reset: true }),
    });
    location.href = "/onboarding.html";
  } catch (e) { alert("could not reset: " + e.message); }
}

function bind() {
  $("btn-start-session").addEventListener("click", startSession);
  $("btn-end-session").addEventListener("click", endSession);
  $("btn-save-config").addEventListener("click", saveConfig);
  const replay = $("btn-replay-onboarding");
  if (replay) replay.addEventListener("click", replayOnboarding);
}

async function boot() {
  bind();
  await timeSync();
  // If the device hasn't been onboarded, redirect to the wizard.
  try {
    const ob = await api("/api/onboarding");
    if (ob && !ob.onboarded && location.pathname !== "/onboarding.html") {
      location.href = "/onboarding.html";
      return;
    }
  } catch (e) {}
  await refreshStatus();
  await refreshConfig();
  await refreshSession();
  await refreshStats();
  setInterval(refreshStatus, 4000);
  setInterval(refreshSession, 3000);
  setInterval(refreshStats, 15000);
}

boot();

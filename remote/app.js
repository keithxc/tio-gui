// SPDX-License-Identifier: GPL-3.0-only
"use strict";
const $ = id => document.getElementById(id);
const client = Array.from(crypto.getRandomValues(new Uint8Array(32)), b => b.toString(16).padStart(2, "0")).join("");
let token = "", active = false, connected = false, session = false, cursor = 0;
let rx = 0, tx = 0, received = new Uint8Array(0), generation = 0;
let viewBytes = received, followOutput = true, pendingRx = 0, viewMode = "text";
let panelEpoch = 0, connectIntent = null, activeDevice = "", faulted = false;
let sending = false, composing = false, claiming = false;
const RETAIN = 128 * 1024;
const finePointer = () => window.matchMedia("(pointer: fine)").matches;
function notice(message, error = false) {
  $("notice").textContent = message; $("notice").title = message;
  $("notice").classList.toggle("error", error);
}
function summary() {
  const device = activeDevice || $("device").value.trim();
  $("session-summary").textContent = device ? device.split(/[\\/]/).pop() : "No port selected";
  $("session-summary").title = device || "Choose a serial port on the gateway computer";
  const parity = ["N", "O", "E"][Number($("parity").value)] || "N";
  $("session-meta").textContent = `${$("baud").value} baud · ${$("bits").value}${parity}${$("stops").value}`;
}
function closeDialog(id) {
  if ($(id).open) {
    if (id === "connection-panel") { panelEpoch++; connectIntent = null; }
    $(id).close();
  }
}
function openSettings() {
  panelEpoch++; connectIntent = null;
  closeDialog("tools-panel");
  if (!$("connection-panel").open) $("connection-panel").showModal();
}
function state(isConnected, hasSession = session) {
  connected = isConnected; session = hasSession;
  $("connection").textContent = connected ? "Connected" : session ? "Connecting / reconnecting" : "Disconnected";
  $("connection").dataset.state = connected ? "connected" : session ? "connecting" : "disconnected";
  for (const id of ["connect", "connect-dialog"]) { $(id).hidden = !active || session; $(id).disabled = !active || session || faulted; }
  $("settings").disabled = session;
  $("disconnect").hidden = !active || !session; $("disconnect").disabled = !active || !session;
  $("send").disabled = !connected || sending || faulted;
  document.querySelectorAll("[data-line]").forEach(button => { button.disabled = !connected; });
  summary();
}
async function api(path, body = {}) {
  const response = await fetch(path, {method: "POST", credentials: "omit", cache: "no-store",
    headers: {"Authorization": `Bearer ${token}`, "X-Tio-Client": client, "Content-Type": "application/json"},
    body: JSON.stringify(body), signal: AbortSignal.timeout(20000)});
  const result = await response.json();
  if (!response.ok || !result.ok) throw new Error(result.error || `Gateway error ${response.status}`);
  return result;
}
function counts() { $("counts").textContent = `RX ${rx.toLocaleString()} B · TX ${tx.toLocaleString()} B`; }
function terminalSelected() {
  const selection = window.getSelection();
  if (!selection || selection.isCollapsed || !selection.rangeCount) return false;
  for (let i = 0; i < selection.rangeCount; i++) {
    if (selection.getRangeAt(i).intersectsNode($("received"))) return true;
  }
  return false;
}
function followIndicator() {
  $("follow").hidden = followOutput && !$("pause").checked && !terminalSelected();
  $("follow").title = pendingRx ? `${pendingRx.toLocaleString()} new bytes; resume following the latest data` : "Resume following latest data";
  $("terminal-empty").hidden = received.length > 0;
}
function render(snapshot = false, resume = false) {
  const area = $("received");
  if (terminalSelected()) { followOutput = false; followIndicator(); return; }
  if (!snapshot && !resume && area.scrollHeight - area.scrollTop - area.clientHeight > 2) followOutput = false;
  if (!snapshot && ($("pause").checked || !followOutput)) { followIndicator(); return; }
  const position = area.scrollTop;
  if (!snapshot) { viewBytes = received; pendingRx = 0; }
  if ($("view").value === "hex") {
    const lines = [];
    for (let i = 0; i < viewBytes.length; i += 16) lines.push(Array.from(viewBytes.subarray(i, i + 16), b => b.toString(16).padStart(2, "0").toUpperCase()).join(" "));
    area.textContent = lines.join("\n");
  } else {
    area.textContent = new TextDecoder().decode(viewBytes).replace(/\x00/g, "␀");
  }
  viewMode = $("view").value;
  area.scrollTop = !snapshot && followOutput ? area.scrollHeight : position;
  followIndicator();
}
function resumeDisplay() {
  if (terminalSelected()) window.getSelection().removeAllRanges();
  $("pause").checked = false; followOutput = true; render(false, true);
}
function decode(data) { return Uint8Array.from(atob(data), char => char.charCodeAt(0)); }
function consume(event) {
  if (event.event === "status") {
    activeDevice = event.device || activeDevice;
    state(event.connected, true); notice(`${event.device || "Serial"}: ${event.message}`);
    if (event.connected && connectIntent) {
      const shouldFinish = connectIntent.epoch === panelEpoch;
      connectIntent = null;
      if (shouldFinish) {
        closeDialog("connection-panel");
        if (finePointer() && !$("tools-panel").open) $("payload").focus({preventScroll: true});
      }
    }
  }
  if (event.event === "done") {
    // A previous close can still be waiting in the event stream when the user
    // starts a new connection. Its DONE must not cancel that new click's intent.
    activeDevice = ""; state(false, false);
    notice("Session closed. Unsent queued data was cancelled.");
  }
  if (event.event === "rx") {
    const bytes = decode(event.data); rx += bytes.length;
    const next = new Uint8Array(Math.min(RETAIN, received.length + bytes.length));
    const previous = Math.min(received.length, Math.max(0, next.length - bytes.length));
    next.set(received.subarray(received.length - previous), 0);
    next.set(bytes.subarray(Math.max(0, bytes.length - next.length)), previous);
    received = next; pendingRx += bytes.length;
  }
  if (event.event === "tx") tx += decode(event.data).length;
}
async function refresh() {
  const result = await api("/api/request", {op: "devices"});
  $("devices").replaceChildren(...result.devices.map(device => {
    const option = document.createElement("option"); option.value = device; return option;
  }));
  if (!$("device").value && result.devices.length) $("device").value = result.devices[0];
  summary();
  notice(`${result.devices.length} remote serial ports found. You can also enter a device path.`);
}
function forget(message, error = false) {
  active = false; generation++; token = ""; connectIntent = null; activeDevice = ""; faulted = false;
  state(false, false); document.body.classList.remove("authenticated");
  closeDialog("connection-panel"); closeDialog("tools-panel");
  $("workspace").hidden = true; $("access").hidden = false;
  notice(message, error);
}
async function poll(mine) {
  try {
    while (active && mine === generation) {
      const result = await api("/api/events", {after: cursor});
      if (!active || mine !== generation) return;
      for (const event of result.events) { consume(event); cursor = event.seq; }
      counts(); if (result.events.some(event => event.event === "rx")) render();
      if (result.fault) {
        faulted = true; connectIntent = null; state(false, false); notice(result.fault, true);
        // Keep the lease and fault visible until the user explicitly releases it.
        await new Promise(resolve => setTimeout(resolve, 3000));
      } else await new Promise(resolve => setTimeout(resolve, 150));
    }
  } catch (error) {
    if (active && mine === generation) {
      try { await api("/api/release"); } catch (_) { /* The lease will expire. */ }
      forget(`Control lost: ${error.message}. Serial closes when the control lease expires.`, true);
    }
  }
}
function action(id, callback) {
  $(id).addEventListener("click", async () => {
    const mine = generation;
    try { await callback(); } catch (error) { if (mine === generation) notice(error.message, true); }
  });
}
$("access-form").addEventListener("submit", async event => {
  event.preventDefault(); if (claiming) return;
  claiming = true; token = $("token").value.trim(); $("token").value = "";
  try {
    await api("/api/claim"); active = true; cursor = 0; generation++;
    faulted = false; activeDevice = ""; rx = tx = 0; received = viewBytes = new Uint8Array(0);
    pendingRx = 0; followOutput = true; $("pause").checked = false;
    if (terminalSelected()) window.getSelection().removeAllRanges();
    counts(); render(false, true); document.body.classList.add("authenticated");
    $("workspace").hidden = false; $("access").hidden = true; state(false, false);
    openSettings(); sizeComposer();
    poll(generation); refresh().catch(error => notice(error.message, true));
  } catch (error) { token = ""; notice(error.message, true); }
  finally { claiming = false; }
});
action("release", async () => {
  active = false; generation++;
  try { await api("/api/release"); forget("Control released. Serial session closed."); }
  catch (error) { forget(`Release could not be confirmed: ${error.message}. Wait for the lease to expire.`, true); }
});
action("refresh", refresh);
async function connect() {
  if (session || faulted || !active) return;
  const request = {op: "open", device: $("device").value.trim(), reconnect: $("reconnect").checked};
  if (!request.device) {
    openSettings(); if (finePointer()) $("device").focus();
    throw new Error("Choose or enter a remote serial port.");
  }
  for (const key of ["baud", "bits", "stops", "parity", "flow"]) request[key] = Number($(key).value);
  const mine = generation;
  connectIntent = {epoch: panelEpoch};
  state(false, true); notice("Opening remote port; waiting for connection status…");
  try { await api("/api/request", request); }
  catch (error) {
    if (mine === generation) { connectIntent = null; state(false, false); }
    throw error;
  }
}
action("connect", connect); action("connect-dialog", connect);
action("disconnect", async () => {
  connectIntent = null;
  await api("/api/request", {op: "close"}); state(false, false);
  notice("Serial session closed. Unsent queued data was cancelled.");
});
action("settings-toggle", openSettings);
for (const id of ["settings-close", "settings-done"]) action(id, () => closeDialog("connection-panel"));
$("connection-panel").addEventListener("close", () => { panelEpoch++; connectIntent = null; });
$("connection-panel").addEventListener("cancel", () => { panelEpoch++; connectIntent = null; });
action("tools-toggle", () => {
  connectIntent = null; closeDialog("connection-panel");
  if (!$("tools-panel").open) $("tools-panel").showModal();
});
action("tools-close", () => closeDialog("tools-panel"));
for (const id of ["device", "baud", "bits", "stops", "parity", "flow"]) $(id).addEventListener("input", summary);
action("clear", () => {
  received = viewBytes = new Uint8Array(0); pendingRx = 0;
  $("received").textContent = ""; followIndicator();
  notice("Received view cleared; session counters retained.");
});
$("pause").addEventListener("change", () => {
  if (!$("pause").checked) resumeDisplay();
  followIndicator();
  notice($("pause").checked ? "Display paused. Receiving continues into the latest 128 KiB buffer." : "Display resumed with the latest buffered data.");
});
action("follow", () => {
  resumeDisplay(); notice("Following the latest received data.");
});
$("received").addEventListener("scroll", () => {
  const area = $("received"), atBottom = area.scrollHeight - area.scrollTop - area.clientHeight <= 2;
  if (!atBottom || terminalSelected()) followOutput = false;
  // A frozen snapshot stays stable even if a format change clamps its scroll.
  // Latest (or unchecking Pause) is the explicit action that resumes live output.
  followIndicator();
}, {passive: true});
document.addEventListener("selectionchange", () => {
  if (terminalSelected()) followOutput = false;
  followIndicator();
});
action("save", () => {
  const url = URL.createObjectURL(new Blob([received], {type: "application/octet-stream"}));
  const link = document.createElement("a"); link.href = url;
  link.download = `tio-rx-${new Date().toISOString().replace(/[:.]/g, "-")}.bin`; link.click();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
  notice(`Download started for the latest ${received.length.toLocaleString()} received bytes. This is a buffer snapshot, not a complete recording.`);
});
$("view").addEventListener("change", () => {
  if (terminalSelected()) {
    $("view").value = viewMode; notice("Clear the terminal selection before changing its view."); return;
  }
  render($("pause").checked || !followOutput);
});
document.querySelectorAll("[data-line]").forEach(button => button.addEventListener("click", async () => {
  try { await api("/api/request", {op: "line", line: Number(button.dataset.line), high: button.dataset.high === "true"}); notice("Serial line request queued."); }
  catch (error) { notice(error.message, true); }
}));
$("send-form").addEventListener("submit", async event => {
  event.preventDefault();
  if (!connected || sending || faulted || composing) return;
  const mine = generation;
  try {
    let bytes;
    if ($("send-mode").value === "hex") {
      const raw = $("payload").value.replace(/\s/g, "");
      if (!/^(?:[0-9a-fA-F]{2})*$/.test(raw)) throw new Error("HEX requires complete byte pairs, optionally separated by spaces.");
      bytes = Uint8Array.from(raw.match(/../g) || [], pair => parseInt(pair, 16));
    } else bytes = new TextEncoder().encode($("payload").value);
    const endings = {"": [], lf: [10], cr: [13], crlf: [13, 10]};
    const ending = endings[$("ending").value], payload = new Uint8Array(bytes.length + ending.length);
    if (!payload.length || payload.length > 65536) throw new Error("Send between 1 and 65,536 bytes, including the line ending.");
    payload.set(bytes); payload.set(ending, bytes.length);
    let binary = ""; for (const byte of payload) binary += String.fromCharCode(byte);
    sending = true; $("send").disabled = true;
    await api("/api/request", {op: "send", data: btoa(binary)});
    if (mine === generation) notice(`${payload.length.toLocaleString()} B queued. TX count advances when writes are confirmed.`);
  } catch (error) { if (mine === generation) notice(error.message, true); }
  finally { sending = false; $("send").disabled = !connected || faulted; }
});
function sizeComposer() {
  const input = $("payload"), style = getComputedStyle(input);
  const line = parseFloat(style.lineHeight) || 20;
  const padding = parseFloat(style.paddingTop) + parseFloat(style.paddingBottom);
  const borders = parseFloat(style.borderTopWidth) + parseFloat(style.borderBottomWidth);
  input.style.height = "auto";
  const maximum = line * 3 + padding + borders;
  input.style.height = `${Math.min(maximum, Math.max(line + padding + borders, input.scrollHeight + borders))}px`;
  input.style.overflowY = input.scrollHeight + borders > maximum ? "auto" : "hidden";
  // Keep a live terminal at its end when the composer or keyboard changes its
  // available height; frozen snapshots and selections retain their position.
  if (followOutput && !$("pause").checked && !terminalSelected()) $("received").scrollTop = $("received").scrollHeight;
}
$("payload").rows = 1;
$("payload").addEventListener("input", sizeComposer);
$("payload").addEventListener("compositionstart", () => { composing = true; });
$("payload").addEventListener("compositionend", () => { composing = false; sizeComposer(); });
$("payload").addEventListener("keydown", event => {
  if (event.key !== "Enter" || event.shiftKey || event.isComposing || composing || event.keyCode === 229) return;
  event.preventDefault();
  if (!event.repeat && connected && !sending && !faulted) $("send-form").requestSubmit();
});
function viewportChanged() {
  const viewport = window.visualViewport;
  if (viewport && Math.abs(viewport.scale - 1) >= 0.05) return;
  document.documentElement.style.setProperty("--workspace-height", `${Math.round(viewport ? viewport.height : window.innerHeight)}px`);
  sizeComposer();
}
window.addEventListener("resize", viewportChanged);
if (window.visualViewport) window.visualViewport.addEventListener("resize", viewportChanged);
window.addEventListener("pagehide", () => {
  if (active && token) {
    fetch("/api/release", {method: "POST", keepalive: true, credentials: "omit",
      headers: {"Authorization": `Bearer ${token}`, "X-Tio-Client": client, "Content-Type": "application/json"}, body: "{}"}).catch(() => {});
  }
});
state(false, false);
followIndicator(); viewportChanged();

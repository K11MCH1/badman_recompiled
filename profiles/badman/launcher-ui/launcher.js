"use strict";
// Badman Recompiled launcher UI. Hosted by WebView2 on Windows and by the Android
// app's WebView (laid out for portrait there). Native code owns files,
// processes and settings storage; this page only renders state and sends user
// intents over the message bridge.

// ------------------------------------------------------------------ settings table
// Only options the runtime actually honours. Platform-specific rows appear
// only in that platform's launcher.
const TABS = [
  { id: "play", label: "Play" },
  { id: "display", label: "Display", title: "DISPLAY", lead: "Window and picture shape. Applied when the game starts." },
  { id: "graphics", label: "Graphics", title: "GRAPHICS", lead: "Rendering API and upscaling." },
  { id: "gameplay", label: "Gameplay", title: "GAMEPLAY", lead: "Start-up, saves and on-screen information." },
  { id: "controls", label: "Controls", title: "CONTROLS", lead: "Controllers, keyboard and touch." },
  { id: "advanced", label: "Advanced", title: "ADVANCED", lead: "Local build, diagnostics and folders." },
  { id: "credits", label: "Credits", title: "CREDITS", lead: "" },
];

const OPTIONS = [
  { page: "display", section: "WINDOW", key: "Fullscreen", type: "bool", def: "0", platforms: ["windows"],
    label: "Fullscreen", help: "Borderless fullscreen on the monitor the game opens on. F11 or Alt+Enter switches while playing." },
  { page: "display", section: "PICTURE SHAPE", key: "Scaling", type: "choice", def: "integer", platforms: ["windows"],
    choices: [["integer", "Integer scale (sharpest)"], ["fit", "Keep PSP shape"], ["stretch", "Stretch to fill"]],
    label: "Picture shape", help: "Integer scale uses whole-pixel multiples when they fit. Keep PSP shape fills the window with bars. Stretch ignores the shape." },
  { page: "display", section: "PICTURE SHAPE", key: "Widescreen", type: "bool", def: "0", platforms: ["windows"],
    label: "Ultra-wide view in the dungeon",
    help: "Fills wide windows (up to 21:9) with more of the dungeon, drawn by the game itself, and moves the HUD to the edges. Removes the dungeon's dark screen-edge vignette. Menus, the title and movies keep the PSP shape. Not with Stretch." },

  { page: "graphics", section: "RENDERER", key: "Renderer", type: "choice", def: "d3d11", platforms: ["windows"],
    choices: [["d3d11", "Direct3D 11"], ["vulkan", "Vulkan"], ["software", "Software"]],
    label: "Rendering API", help: "Switching does not need another build. Choose another API if your driver lacks one." },
  { page: "graphics", section: "RENDERER", key: "AndroidRenderer", type: "choice", def: "cpu", platforms: ["android"],
    choices: [["cpu", "CPU (all cores)"], ["vulkan", "Vulkan"], ["software", "Software"]],
    label: "Rendering API", help: "CPU draws on the phone's spare cores. Vulkan draws on the GPU. Software uses the game thread only. Switching does not need another build." },
  { page: "graphics", section: "MOTION", key: "Interpolation", type: "bool", def: "1", platforms: ["windows"],
    label: "Smooth motion up to your refresh rate",
    help: "Draws in-between frames on every display refresh (for example 165 Hz) while the game keeps its own 30 FPS pace. Adds one game frame of display delay." },
  { page: "graphics", section: "MOTION", key: "Interpolation", type: "bool", def: "0", platforms: ["android"],
    label: "Smooth motion up to your refresh rate",
    help: "Draws in-between frames on every screen refresh (up to 144 Hz on a 144 Hz phone) while the game keeps its own 30 FPS pace. Adds one game frame of display delay." },
  { page: "graphics", section: "UPSCALING", key: "Filter", type: "choice", def: "nearest",
    choices: [["nearest", "Sharp pixels"], ["smooth", "Smooth"]],
    label: "Upscaling filter", help: "Sharp pixels keeps every PSP pixel crisp. Smooth blends pixels when the picture is scaled." },

  { page: "gameplay", section: "START-UP", key: "SkipIntro", type: "bool", def: "0",
    label: "Skip intro and logos", help: "Skips the boot logo movie through the game's own cancel path." },
  { page: "gameplay", section: "SAVES", key: "AutoProfile", type: "bool", def: "1", platforms: ["windows"],
    label: "Automatic save profile", help: "Answers the Memory Stick prompt and saves to your one profile in the Saves folder, without a slot chooser. Delete still asks." },
  { page: "gameplay", section: "ON SCREEN", key: "FpsCounter", type: "bool", def: "1", platforms: ["windows"],
    label: "FPS counter", help: "Shows the game's frames per second in the top-left corner." },

  { page: "controls", section: "CONTROLLER", key: "Controller", type: "bool", def: "1",
    label: "Game controllers", help: "Xbox controllers and the Steam Deck work directly (XInput). DualSense and Switch Pro work through Steam Input or DS4Windows." , platforms: ["windows"] },
  { page: "controls", section: "CONTROLLER", key: "StickDeadzone", type: "int", def: "20", min: 0, max: 50, step: 5, unit: "%",
    label: "Stick dead zone", help: "How far the stick must move before the cursor moves." },
  { page: "controls", section: "TOUCH", key: "AndroidTouch", type: "bool", def: "1", platforms: ["android"],
    label: "On-screen touch controls", help: "A stick for the cursor and menus, the face buttons, L, R, Start and Select. Hidden while a controller is connected." },

  { page: "advanced", section: "LOCAL BUILD", key: "Jobs", type: "choice", def: "2", platforms: ["windows"],
    choices: [["1", "1"], ["2", "2"], ["3", "3"], ["4", "4"]],
    label: "Parallel compile jobs", help: "More jobs build faster but use more memory (about 1–2 GB each)." },
  { page: "advanced", section: "LOCAL BUILD", action: "verify", label: "Verify the local build", platforms: ["windows"], help: "Checks your prepared files and compiled game without changing anything." },
  { page: "advanced", section: "LOCAL BUILD", action: "verify", label: "Verify the local build", platforms: ["android"],
    help: "Checks that the prepared files and the compiled game are on this phone and match this version of the app." },
  { page: "advanced", section: "LOCAL BUILD", action: "reprepare", label: "Prepare again from your ISO", platforms: ["android"],
    help: "Extracts, decrypts and compiles the game again from the ISO chosen on the Play page. Your saves stay." },
  { page: "advanced", section: "FOLDERS", action: "openData", label: "Open the game data folder", platforms: ["windows"], help: "" },
  { page: "advanced", section: "FOLDERS", action: "openLogs", label: "Open the launcher logs", platforms: ["windows"], help: "" },
];

const INFO = {
  controls: [
    { section: "KEYBOARD", platforms: ["windows"], rows: [["D-pad", "Arrow keys"], ["Cross / Circle", "Z / X"], ["Square / Triangle", "A / S"],
      ["Start / Select", "Enter / Backspace"], ["L / R", "Q / E"]] },
    { section: "CONTROLLER BUTTONS", rows: [["Cross / Circle", "A / B"], ["Square / Triangle", "X / Y"], ["Start / Select", "Start (Menu) / Back (View)"],
      ["L / R", "LB / RB"], ["Analog stick / D-pad", "Left stick / D-pad"]] },
    { section: "LAUNCHER", platforms: ["windows"], rows: [["Move", "Arrow keys, D-pad or stick"], ["Select", "Enter or A"], ["Back", "Esc or B"], ["Previous / next tab", "LB / RB"]] },
    { section: "LAUNCHER", platforms: ["android"], rows: [["Change a setting", "Tap it; ◀ and ▶ step back and forth"], ["Move", "D-pad or stick"],
      ["Select", "A"], ["Back", "B or the Back gesture"], ["Previous / next tab", "LB / RB"]] },
  ],
};

// {name} is the product name ("Badman Recompiled"; the host may send another).
const CREDITS = [
  { text: "<b>{name}</b> — a static recompilation of your own copy of <i>Holy Invasion of Privacy, Badman! What Did I Do to Deserve This?</i> (ULUS10456, version 1.01). No game files are included; your ISO is prepared and compiled on this device." },
  { text: "<b>PSPRecomp</b> framework by Jessica_Natalia, MIT licence. Badman profile, native runtime and launcher: PSPRecomp Badman contributors." },
  { text: "<b>FFmpeg</b> (LGPL 2.1+) decodes movies and music. <b>pspdecrypt</b> by John-K (GPLv3) runs as a separate program to decrypt your own executable." },
  { text: "<b>Vulkan-Headers</b> (Apache-2.0 / MIT), <b>Microsoft WebView2 SDK</b> (BSD), <b>Python</b> (PSF licence).", platforms: ["windows"] },
  { text: "<b>LLVM Clang and LLD</b> (Apache-2.0 with LLVM exceptions, packaged by Termux) compile your game on this phone, with the <b>Android NDK</b> headers and C++ runtime.", platforms: ["android"] },
  { text: "<b>Pixelify Sans</b> by Stefie Justprince (SIL Open Font Licence). Replacement PSP fonts are Ume-derived open-licence fonts." },
  { text: "<i>Holy Invasion of Privacy, Badman!</i> is © 2009 Sony Computer Entertainment Inc. This project is not affiliated with or endorsed by Sony." },
];

// ------------------------------------------------------------------ host bridge
const webview = window.chrome && window.chrome.webview;
const android = window.BadmanAndroid; // The Android app's JavaScript interface.
const host = webview ? {
  send: (message) => webview.postMessage(message),
  listen: (handler) => webview.addEventListener("message", (event) => handler(event.data)),
} : android ? {
  send: (message) => android.postMessage(JSON.stringify(message)),
  listen: (handler) => { window.badmanReceive = handler; },
} : mockHost();

function mockHost() {
  // Browser preview without native code: realistic data, no side effects.
  let listener = null;
  const params = new URLSearchParams(location.search);
  const platform = params.get("platform") || "windows";
  const settings = platform === "android" ? { ISO: "Badman.iso", DataDirectory: "App storage on this phone", SkipIntro: "1" }
    : { ISO: "D:\\Games\\Badman.iso", DataDirectory: "C:\\Users\\you\\AppData\\Local\\BadmanRecomp\\game", Trace: "1", SkipIntro: "0" };
  setTimeout(() => listener && listener({ type: "init", platform, name: platform === "android" ? "Badman Recompiled" : undefined, progress: 1,
    settings, art: params.get("art") || "art/", status: "Local build verified. Press Play to launch.", statusKind: "ok" }), 30);
  return {
    send(message) {
      if (message.type === "set") settings[message.key] = message.value;
      if (platform === "android" && message.type === "prepare") {
        // Walks the progress bar from 0 to 100% in about ten seconds.
        listener({ type: "busy", busy: true, play: false, clear: true });
        let value = 0;
        const timer = setInterval(() => {
          value = Math.min(1, value + 0.004 + Math.random() * 0.012);
          listener({ type: "progress", value, active: value < 1 });
          listener({ type: "status", text: `Compiling the game: ${Math.round(value * 74)} of 74 parts`, kind: "" });
          if (value >= 1) { clearInterval(timer); listener({ type: "finished", code: 0, play: false }); }
        }, 120);
        return;
      }
      if (message.type === "play" || message.type === "prepare") {
        listener({ type: "busy", busy: true, play: message.type === "play" });
        listener({ type: "log", text: "Preview mode: the native launcher runs setup here.\n" });
        setTimeout(() => listener({ type: "finished", code: 0, play: message.type === "play" }), 1200);
      }
    },
    listen(handler) { listener = handler; },
  };
}

// ------------------------------------------------------------------ state
const state = {
  platform: "windows", name: "Badman Recompiled", settings: {}, art: "https://art.local/", tab: "play", zone: "page",
  focus: {}, busy: false, status: "", statusKind: "", log: "", logExpanded: false, ready: false,
  progress: 0, progressActive: false, // Preparation progress (0..1), sent by the Android host.
};
const value = (key, def) => (state.settings[key] !== undefined && state.settings[key] !== "" ? state.settings[key] : def);
const visible = (row) => !row.platforms || row.platforms.includes(state.platform);
// A settings tab with nothing for this platform (Display on Android) is left out.
const tabs = () => TABS.filter((tab) => tab.id === "play" || tab.id === "credits" ||
  OPTIONS.some((row) => row.page === tab.id && visible(row)) || (INFO[tab.id] || []).some(visible));
function set(key, next) {
  state.settings[key] = String(next);
  host.send({ type: "set", key, value: String(next) });
}

// ------------------------------------------------------------------ rendering
const $ = (selector) => document.querySelector(selector);
const element = (tag, className, html) => { const node = document.createElement(tag); if (className) node.className = className; if (html !== undefined) node.innerHTML = html; return node; };
const escapeHtml = (text) => String(text).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
let items = [];

function renderBackgrounds() {
  const root = $("#backgrounds");
  root.innerHTML = "";
  for (const tab of tabs()) {
    const layer = element("div", "bg" + (tab.id === "play" ? "" : " blurred"));
    layer.dataset.tab = tab.id;
    const stage = element("div", "stage");
    const image = new Image();
    image.alt = "";
    image.onerror = () => layer.classList.add("missing");
    image.onload = () => {
      // Art keeps its own shape: the native 480x272 frame, or a larger redraw
      // (for example 1441x720 without the copyright bar). Positions on it are
      // in game pixels, 480 across; larger art is filtered smoothly.
      stage.style.setProperty("--ar", image.naturalWidth / image.naturalHeight);
      stage.style.setProperty("--gh", 480 * image.naturalHeight / image.naturalWidth);
      layer.classList.toggle("hd", image.naturalWidth > 720);
    };
    image.src = state.art + tab.id + ".png";
    stage.append(image);
    // On Android the portrait Play art carries its own title lines.
    if (tab.id === "play" && state.platform !== "android") stage.append(element("div", "subtitle", "What Did I Do To Deserve This Recompile?"));
    layer.append(stage);
    root.append(layer);
  }
  updateBackground();
}
function updateBackground() {
  for (const layer of document.querySelectorAll(".bg")) layer.classList.toggle("active", layer.dataset.tab === state.tab);
}

function renderTabs() {
  const nav = $("#tabs");
  nav.innerHTML = "";
  tabs().forEach((tab, index) => {
    const button = element("button", "tab", escapeHtml(tab.label));
    button.style.setProperty("--i", index);
    button.dataset.tab = tab.id;
    button.setAttribute("role", "tab");
    button.addEventListener("click", () => { selectTab(tab.id); state.zone = "tabs"; updateFocus(); });
    button.addEventListener("mouseenter", () => { if (state.zone === "tabs") { selectTab(tab.id); } });
    nav.append(button);
  });
  updateTabs();
}
function updateTabs() {
  for (const button of document.querySelectorAll(".tab")) {
    const current = button.dataset.tab === state.tab;
    button.classList.toggle("current", current);
    button.setAttribute("aria-selected", current);
  }
}

function addItem(node, spec) {
  node.addEventListener("mouseenter", () => { state.zone = "page"; setFocus(items.indexOf(spec)); });
  node.addEventListener("click", (event) => {
    event.preventDefault(); state.zone = "page"; setFocus(items.indexOf(spec));
    // A click or tap on ◀ steps back; anywhere else steps forward.
    const arrow = event.target.closest && event.target.closest(".arrow");
    activate(spec, arrow && arrow.textContent === "◀" ? -1 : +1);
  });
  spec.node = node;
  items.push(spec);
  return node;
}

function optionText(option) {
  const current = value(option.key, option.def);
  if (option.type === "bool") return `<span class="switch${current === "1" ? " on" : ""}"></span>`;
  if (option.type === "int") return `<span class="arrow">◀</span>${escapeHtml(current)}${option.unit || ""}<span class="arrow">▶</span>`;
  const choice = option.choices.find(([id]) => id === current) || option.choices[0];
  return `<span class="arrow">◀</span>${escapeHtml(choice[1])}<span class="arrow">▶</span>`;
}

function renderSettingsPage(tab) {
  const page = $("#page");
  page.append(element("h1", "page-title", tab.title), element("p", "page-lead", escapeHtml(tab.lead)));
  const sections = element("div", "sections");
  const help = element("div", "help");
  let lastSection = null;
  const sectionTitle = (name) => { if (name !== lastSection) { sections.append(element("div", "section-title", escapeHtml(name))); lastSection = name; } };
  for (const option of OPTIONS.filter((row) => row.page === tab.id && visible(row))) {
    sectionTitle(option.section);
    const row = element("div", "row");
    row.append(element("span", "label", escapeHtml(option.label)));
    const valueNode = element("span", "value", option.action ? '<span class="arrow">▶</span>' : optionText(option));
    row.append(valueNode);
    sections.append(addItem(row, { kind: option.action ? "action" : "option", option, valueNode, help: option.help, group: items.length }));
  }
  for (const block of (INFO[tab.id] || []).filter(visible)) {
    sectionTitle(block.section);
    for (const [label, text] of block.rows) {
      const row = element("div", "row info");
      row.append(element("span", "label", escapeHtml(label)), element("span", "value", escapeHtml(text)));
      sections.append(row);
    }
  }
  page.append(sections, help);
  state.helpNode = help;
}

function renderCredits() {
  const page = $("#page");
  page.append(element("h1", "page-title", "CREDITS"));
  const body = element("div", "sections credits");
  for (const paragraph of CREDITS.filter(visible)) body.append(element("p", "", paragraph.text.replace("{name}", escapeHtml(state.name))));
  page.append(body);
}

function pathText(text, fallback) {
  if (!text) return fallback;
  return text.length > 52 ? "…" + text.slice(-50) : text;
}

function renderPlay() {
  const page = $("#page");
  const wrapper = element("div", "play-page");
  const panel = element("div", "play-panel");
  const left = element("div", "play-left");
  const middle = element("div", "play-middle");
  const right = element("div", "play-right");
  const play = element("button", "play-button" + (state.busy ? " busy" : ""), state.busy ? "WORKING…" : "PLAY");
  left.append(addItem(play, { kind: "action", option: { action: "play" }, group: 0, column: 0 }));
  const pickerRow = (label, key, target, group) => {
    const row = element("div", "row");
    // The value cuts long paths at the start (right-to-left box); <bdi> keeps the text itself left to right.
    row.append(element("span", "label", label), element("span", "value", "<bdi>" + escapeHtml(value(key, "") || "Choose…") + "</bdi>"));
    row.title = value(key, "");
    return addItem(row, { kind: "browse", target, group, column: 1 });
  };
  middle.append(pickerRow("Game ISO", "ISO", "iso", 1));
  if (state.platform === "android") {
    // The game data lives in the app's own storage; there is no folder to choose.
    const dataRow = element("div", "row info");
    dataRow.append(element("span", "label", "Game data"), element("span", "value", escapeHtml(value("DataDirectory", ""))));
    middle.append(dataRow);
  } else {
    middle.append(pickerRow("Game data folder", "DataDirectory", "data", 2));
    const traceRow = element("div", "row");
    const traceOption = { key: "Trace", type: "bool", def: "1" };
    const traceValue = element("span", "value", optionText(traceOption));
    traceRow.append(element("span", "label", "Save diagnostic trace"), traceValue);
    middle.append(addItem(traceRow, { kind: "option", option: traceOption, valueNode: traceValue, group: 3, column: 1 }));
  }
  const actions = element("div", "actions");
  const buttons = state.platform === "android" ? [["Prepare and build", "prepare"], ["Cancel", "cancel"]]
    : [["Prepare and build", "prepare"], ["Open Saves", "openSaves"]];
  for (const [label, action] of buttons)
    actions.append(addItem(element("button", "action", label), { kind: "action", option: { action }, group: 4, column: 1 }));
  middle.append(actions);
  const status = element("div", "status" + (state.statusKind ? " " + state.statusKind : ""), escapeHtml(state.status));
  right.append(status);
  state.statusNode = status; state.playNode = play;
  if (state.platform === "android") {
    // The phone shows how far preparation has got instead of a log.
    right.append(renderDig());
  } else {
    const progress = element("div", "progress" + (state.busy ? " on" : ""));
    const log = element("div", "log" + (state.logExpanded ? " expanded" : ""));
    log.textContent = state.log || "Preparation and runtime log";
    right.append(progress, addItem(log, { kind: "log", group: 5, column: 2 }));
    state.logNode = log; state.progressNode = progress;
  }
  panel.append(left, middle, right);
  wrapper.append(panel);
  page.append(wrapper);
  if (state.logNode) state.logNode.scrollTop = state.logNode.scrollHeight;
}

// ------------------------------------------------------------------ progress with dust (Android)
// A dig bar: the fill advances with the preparation, its leading edge kicks
// up pixel dust that arcs back down onto the bar, and the label gives the
// percentage. Dust only flies while a job runs (and not with reduced motion).
const dig = { node: null, fill: null, label: null, canvas: null, particles: [], frame: 0, last: 0, spawn: 0 };
const DUST = ["#ffffff", "#cfe6f4", "#efe3c8", "#d7c49c", "#a58f68"];
const reducedMotion = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

function renderDig() {
  const node = element("div", "dig");
  node.setAttribute("role", "progressbar");
  node.setAttribute("aria-valuemin", "0");
  node.setAttribute("aria-valuemax", "100");
  const fill = element("div", "dig-fill"), canvas = element("canvas", "dig-dust"), label = element("div", "dig-label");
  node.append(fill, canvas, label);
  Object.assign(dig, { node, fill, label, canvas, particles: [] });
  updateDig();
  return node;
}
function updateDig() {
  if (!dig.node) return;
  const percent = Math.max(0, Math.min(100, Math.floor(state.progress * 100)));
  dig.node.style.setProperty("--p", Math.max(0, Math.min(1, state.progress)));
  dig.node.classList.toggle("active", state.progressActive);
  dig.node.classList.toggle("failed", !state.progressActive && state.statusKind === "bad");
  dig.node.setAttribute("aria-valuenow", percent);
  dig.label.textContent = percent + "%";
  if (state.progressActive) startDust();
}
function startDust() {
  if (reducedMotion || dig.frame) return;
  dig.last = performance.now();
  dig.frame = requestAnimationFrame(stepDust);
}
function addDust(x, y, spread) {
  const life = 0.55 + Math.random() * 0.9;
  dig.particles.push({
    x: x + (Math.random() - 0.5) * spread, y, life, max: life,
    vx: -(15 + Math.random() * 95) * (spread > 8 ? Math.random() - 0.5 : 1), vy: -(45 + Math.random() * 135),
    size: Math.random() < 0.2 ? 3 : 2, color: DUST[Math.floor(Math.random() * DUST.length)],
  });
}
// A last puff along the whole bar when preparation finishes.
function burstDust() {
  if (reducedMotion || !dig.canvas || !dig.canvas.isConnected) return;
  const width = dig.canvas.clientWidth, bar = dig.node.clientHeight, top = dig.canvas.clientHeight - bar;
  for (let i = 0; i < 70; i++) addDust(Math.random() * width, top + bar * (0.2 + Math.random() * 0.6), 12);
  startDust();
}
function stepDust(time) {
  dig.frame = 0;
  const canvas = dig.canvas;
  if (!canvas || !canvas.isConnected) { dig.particles = []; return; }
  const dt = Math.min(0.05, (time - dig.last) / 1000);
  dig.last = time;
  const ratio = window.devicePixelRatio || 1, width = canvas.clientWidth, height = canvas.clientHeight;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
    canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio);
  }
  const bar = dig.node.clientHeight, top = height - bar, edge = dig.fill.offsetWidth;
  if (state.progressActive && edge > 2) {
    for (dig.spawn += dt * 85; dig.spawn >= 1; dig.spawn -= 1) addDust(edge, top + bar * (0.25 + Math.random() * 0.5), 4);
  }
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  const floor = top + bar - 4;
  dig.particles = dig.particles.filter((p) => {
    p.life -= dt;
    p.vy += 330 * dt;
    p.vx *= 1 - 1.6 * dt;
    p.x += p.vx * dt; p.y += p.vy * dt;
    if (p.y > floor && p.vy > 0) { p.y = floor; p.vy *= -0.28; p.vx *= 0.5; } // Settles on the bar.
    if (p.life <= 0 || p.x < -4 || p.x > width + 4) return false;
    context.globalAlpha = Math.min(1, p.life / p.max * 1.6);
    context.fillStyle = p.color;
    context.fillRect(Math.round(p.x), Math.round(p.y), p.size, p.size);
    return true;
  });
  context.globalAlpha = 1;
  if (state.progressActive || dig.particles.length) dig.frame = requestAnimationFrame(stepDust);
}

function renderPage(animate = true) {
  const page = $("#page");
  page.innerHTML = "";
  page.classList.remove("page-enter");
  items = []; state.helpNode = null; state.logNode = null;
  const tab = TABS.find((t) => t.id === state.tab);
  if (tab.id === "play") renderPlay();
  else if (tab.id === "credits") renderCredits();
  else renderSettingsPage(tab);
  if (animate) { void page.offsetWidth; page.classList.add("page-enter"); }
  updateFocus();
}

// ------------------------------------------------------------------ focus and input
const hand = element("span", "hand");
function setFocus(index) {
  if (!items.length) return;
  state.focus[state.tab] = Math.max(0, Math.min(items.length - 1, index));
  updateFocus();
}
function updateFocus() {
  for (const node of document.querySelectorAll(".focus")) node.classList.remove("focus");
  hand.remove();
  if (state.zone === "tabs" || !items.length) {
    const button = document.querySelector(`.tab[data-tab="${state.tab}"]`);
    if (button) { button.classList.add("focus"); button.prepend(hand); }
    if (state.helpNode) state.helpNode.textContent = "";
    return;
  }
  const index = Math.min(state.focus[state.tab] ?? defaultFocus(), items.length - 1);
  state.focus[state.tab] = index;
  const item = items[index];
  item.node.classList.add("focus");
  item.node.prepend(hand);
  item.node.scrollIntoView({ block: "nearest" });
  if (state.helpNode) state.helpNode.textContent = item.help || "";
}
function defaultFocus() {
  const play = items.findIndex((item) => item.option && item.option.action === "play");
  return play >= 0 ? play : 0;
}
function selectTab(id) {
  if (state.tab === id) return;
  state.tab = id; state.logExpanded = false;
  updateTabs(); updateBackground(); renderPage();
}
function moveTab(step) {
  const shown = tabs();
  const index = shown.findIndex((t) => t.id === state.tab);
  selectTab(shown[(index + step + shown.length) % shown.length].id);
}
function moveVertical(step) {
  const index = state.focus[state.tab] ?? defaultFocus();
  const item = items[index];
  // Items in a column layout move within their column; others move by group.
  const pool = items.filter((other) => other.column === item.column);
  const groups = [...new Set(pool.map((other) => other.group))];
  const position = groups.indexOf(item.group) + step;
  if (position >= groups.length) { state.zone = "tabs"; updateFocus(); return; }
  if (position < 0) return;
  setFocus(items.indexOf(pool.find((other) => other.group === groups[position])));
}
function moveColumn(step) {
  const item = items[state.focus[state.tab] ?? defaultFocus()];
  if (item.column === undefined) return false;
  const columns = [...new Set(items.map((other) => other.column))].sort();
  const target = columns[columns.indexOf(item.column) + step];
  if (target === undefined) return true;
  const top = item.node.getBoundingClientRect().top;
  const candidates = items.filter((other) => other.column === target);
  candidates.sort((a, b) => Math.abs(a.node.getBoundingClientRect().top - top) - Math.abs(b.node.getBoundingClientRect().top - top));
  setFocus(items.indexOf(candidates[0]));
  return true;
}
function activate(item, step) {
  if (item.kind === "browse") { if (!state.busy) host.send({ type: "browse", target: item.target }); return; }
  if (item.kind === "log") { state.logExpanded = !state.logExpanded; item.node.classList.toggle("expanded", state.logExpanded); return; }
  const option = item.option;
  if (item.kind === "action") {
    if (state.busy && !["openSaves", "openData", "openLogs", "cancel"].includes(option.action)) { toast("Please wait for the current job to finish."); return; }
    host.send({ type: option.action });
    return;
  }
  if (state.busy) { toast("Settings can be changed after the current job finishes."); return; }
  const current = value(option.key, option.def);
  if (option.type === "bool") set(option.key, current === "1" ? "0" : "1");
  else if (option.type === "int") set(option.key, Math.max(option.min, Math.min(option.max, Number(current) + step * option.step)));
  else {
    const index = option.choices.findIndex(([id]) => id === current);
    set(option.key, option.choices[(Math.max(0, index) + step + option.choices.length) % option.choices.length][0]);
  }
  item.valueNode.innerHTML = optionText(option);
}
function adjust(step) {
  const item = items[state.focus[state.tab] ?? defaultFocus()];
  if (!item) return;
  if (item.kind === "option") { activate(item, step); return; }
  const siblings = items.filter((other) => other.group === item.group);
  const at = siblings.indexOf(item) + step;
  if (siblings.length > 1 && at >= 0 && at < siblings.length) { setFocus(items.indexOf(siblings[at])); return; }
  moveColumn(step);
}

function input(command) {
  if (state.zone === "tabs") {
    if (command === "left") moveTab(-1);
    else if (command === "right") moveTab(+1);
    else if (command === "up" || command === "enter") { if (items.length) { state.zone = "page"; updateFocus(); } }
    else if (command === "prevTab") moveTab(-1);
    else if (command === "nextTab") moveTab(+1);
    updateFocus();
    return;
  }
  if (!items.length) { state.zone = "tabs"; updateFocus(); return; }
  const item = items[state.focus[state.tab] ?? defaultFocus()];
  if (state.logExpanded && command !== "enter" && command !== "back") return;
  switch (command) {
    case "up": moveVertical(-1); break;
    case "down": moveVertical(+1); break;
    case "left": adjust(-1); break;
    case "right": adjust(+1); break;
    case "enter": activate(item, +1); break;
    case "back":
      if (state.logExpanded) { state.logExpanded = false; item.node.classList.remove("expanded"); break; }
      state.zone = "tabs"; updateFocus(); break;
    case "prevTab": moveTab(-1); break;
    case "nextTab": moveTab(+1); break;
  }
}

const KEYS = { ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right", Enter: "enter", " ": "enter", Escape: "back",
  Backspace: "back", PageUp: "prevTab", PageDown: "nextTab" };
window.addEventListener("keydown", (event) => {
  const command = KEYS[event.key];
  if (!command) return;
  event.preventDefault();
  input(command);
});

// Gamepads through the standard Gamepad API (XInput, DualSense and others in WebView2).
const pad = { held: {}, next: {} };
function pollPads(time) {
  const pads = navigator.getGamepads ? Array.from(navigator.getGamepads()).filter(Boolean) : [];
  const pressed = {};
  for (const gamepad of pads) {
    const b = (i) => gamepad.buttons[i] && gamepad.buttons[i].pressed;
    const x = gamepad.axes[0] || 0, y = gamepad.axes[1] || 0;
    pressed.up ||= b(12) || y < -0.55; pressed.down ||= b(13) || y > 0.55;
    pressed.left ||= b(14) || x < -0.55; pressed.right ||= b(15) || x > 0.55;
    pressed.enter ||= b(0); pressed.back ||= b(1); pressed.prevTab ||= b(4); pressed.nextTab ||= b(5);
  }
  for (const command of ["up", "down", "left", "right", "enter", "back", "prevTab", "nextTab"]) {
    if (!pressed[command]) { pad.held[command] = false; continue; }
    const repeating = ["up", "down", "left", "right"].includes(command);
    if (!pad.held[command]) { pad.held[command] = true; pad.next[command] = time + 380; input(command); }
    else if (repeating && time >= pad.next[command]) { pad.next[command] = time + 110; input(command); }
  }
  requestAnimationFrame(pollPads);
}

// ------------------------------------------------------------------ host messages
let toastTimer = 0;
function toast(text) {
  const node = $("#toast");
  node.textContent = text; node.classList.add("show");
  clearTimeout(toastTimer); toastTimer = setTimeout(() => node.classList.remove("show"), 2600);
}
function appendLog(text) {
  state.log += text;
  if (state.log.length > 400000) state.log = state.log.slice(-300000);
  if (state.logNode) { state.logNode.textContent = state.log; state.logNode.scrollTop = state.logNode.scrollHeight; }
}
function setStatus(text, kind) {
  state.status = text || ""; state.statusKind = kind || "";
  if (state.statusNode) { state.statusNode.textContent = state.status; state.statusNode.className = "status" + (kind ? " " + kind : ""); }
  updateDig();
}
function setBusy(busy, play) {
  state.busy = busy;
  if (state.progressNode) state.progressNode.classList.toggle("on", busy);
  if (state.playNode) {
    state.playNode.classList.toggle("busy", busy);
    for (const node of state.playNode.childNodes) if (node.nodeType === Node.TEXT_NODE) node.textContent = busy ? (play ? "STARTING…" : "WORKING…") : "PLAY";
  }
}

host.listen((message) => {
  switch (message.type) {
    case "init":
      state.platform = message.platform || "windows";
      if (message.name) { state.name = message.name; document.title = message.name; }
      if (!tabs().some((tab) => tab.id === state.tab)) state.tab = "play";
      document.documentElement.dataset.platform = state.platform;
      if (message.progress !== undefined) state.progress = Number(message.progress) || 0;
      state.settings = message.settings || {};
      if (message.art) state.art = message.art;
      setStatus(message.status || "", message.statusKind || "");
      renderBackgrounds(); renderTabs(); renderPage(false);
      break;
    case "settings":
      state.settings = message.settings || {};
      renderPage(false);
      break;
    case "log": appendLog(message.text || ""); break;
    case "status": setStatus(message.text, message.kind); break;
    case "busy":
      setBusy(true, message.play);
      setStatus(message.play ? "Verifying your local build, then launching…" : "Preparing game files and compiling your local game…", "");
      if (message.clear) { state.log = ""; if (state.logNode) state.logNode.textContent = ""; }
      break;
    case "finished":
      setBusy(false, message.play);
      if (message.code === 0) setStatus(message.play ? "Game session finished." : "Local build verified. Press Play to launch.", "ok");
      else if (state.platform === "android") setStatus("Preparation failed.", "bad"); // The host sends the reason next.
      else setStatus(message.play ? "The session stopped. See the log." : "Preparation failed. See the log.", "bad");
      if (message.code === 0 && !message.play) burstDust();
      break;
    case "progress":
      state.progress = Number(message.value) || 0;
      state.progressActive = !!message.active;
      updateDig();
      break;
    case "toast": toast(message.text || ""); break;
    case "selectTab": state.zone = "tabs"; selectTab(message.tab); updateFocus(); break;
  }
});

// The Android Back button or gesture: true when the page handled it (an
// expanded log closes, another tab returns to Play); false lets the app close.
window.badmanBack = () => {
  if (state.logExpanded) { input("back"); return true; }
  if (state.tab !== "play") { state.zone = "page"; selectTab("play"); return true; }
  return false;
};

requestAnimationFrame(pollPads);
host.send({ type: "ready" });

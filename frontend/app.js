/* LG Input Mapper TV app. One render() function draws the current state into
 * #app; the remote's D-pad drives focus via nav.js. */
var api = require('./api');
var nav = require('./nav');
var keys = require('../shared/keys');

var KEYS = keys.KEYS;
var NOISE = keys.NOISE_CODES;
var VERSION = typeof __APP_VERSION__ !== 'undefined' ? __APP_VERSION__ : 'dev';
var COUNTDOWN_MS = 10000;
var BACK = 412;
// Keys the app itself needs while listening for a *target* button; they pass
// through the daemon and are never taken as the result (they are in the list).
var NAV_KEYS = [412, 28, 272, 103, 108, 105, 106];
var MONITOR_CAPTURE_MS = 30000;

// Buttons that can never be the source of a mapping, so the TV and this app
// always stay controllable.
var EXCLUDED = { 103: 1, 108: 1, 105: 1, 106: 1, 28: 1, 272: 1, 412: 1, 773: 1, 139: 1, 116: 1, 115: 1, 114: 1, 402: 1, 403: 1 };
var EXCLUDED_LIST = Object.keys(EXCLUDED).map(function (k) { return parseInt(k, 10); });

var ICON = {
  plus: '<svg class="ic" viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></svg>',
  gear: '<svg class="ic" viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/></svg>',
  block: '<svg class="ic" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M5.6 5.6l12.8 12.8"/></svg>',
  swap: '<svg class="ic" viewBox="0 0 24 24"><path d="M17 3l4 4-4 4M21 7H8M7 21l-4-4 4-4M3 17h13"/></svg>',
  play: '<svg class="ic" viewBox="0 0 24 24"><path d="M6 4l14 8-14 8z"/></svg>',
  hdmi: '<svg class="ic" viewBox="0 0 24 24"><rect x="2" y="7" width="20" height="10" rx="2"/><path d="M6 11h12M6 14h8"/></svg>',
  term: '<svg class="ic" viewBox="0 0 24 24"><path d="M4 17l6-5-6-5M12 19h8"/></svg>',
  none: '<svg class="ic" viewBox="0 0 24 24"><path d="M5 12h14"/></svg>',
  hold: '<svg class="ic" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 3"/></svg>',
  check: '<svg class="ic" viewBox="0 0 24 24"><path d="M5 12l5 5L20 7"/></svg>',
  edit: '<svg class="ic" viewBox="0 0 24 24"><path d="M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/></svg>',
  power: '<svg class="ic" viewBox="0 0 24 24"><path d="M18.4 6.6a9 9 0 1 1-12.8 0M12 2v10"/></svg>',
  trash: '<svg class="ic" viewBox="0 0 24 24"><path d="M3 6h18M8 6V4h8v2M19 6l-1 14H6L5 6M10 11v6M14 11v6"/></svg>',
};

var COMMAND_PRESETS = [
  { title: 'Turn the screen off', desc: 'Picture off, audio keeps playing', command: 'luna-send -n 1 luna://com.webos.service.tvpower/power/turnOffScreen \'{"standbyMode":"active"}\'' },
  { title: 'Show a notification', desc: 'Toast message on screen', command: 'luna-send -n 1 luna://com.webos.notification/createToast \'{"message":"Hello from LG Input Mapper"}\'' },
  { title: 'Call a webhook', desc: 'HTTP request, e.g. Home Assistant', command: 'curl -s -X POST http://homeassistant.local:8123/api/webhook/lg-remote' },
  { title: 'Open Settings', desc: 'Launch the settings app', command: 'luna-send -n 1 luna://com.webos.applicationManager/launch \'{"id":"com.palm.app.settings"}\'' },
];

// Buttons worth offering as replacement targets even though the remote may not
// have them.
var TARGET_LIST = [164, 207, 119, 128, 167, 168, 208, 163, 165, 113, 358, 174, 362, 241, 994, 1083, 771,
  2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 398, 399, 400, 401, 773, 139, 412, 28, 103, 108, 105, 106, 115, 114, 402, 403,
  1037, 1038, 1042, 1043, 1044, 1111, 1086, 1117, 428, 1023, 1124];

var S = {
  screen: 'boot',        // boot | error | main | flow | settings | monitor | about
  bootLog: [],
  error: null,
  status: null,
  config: null,
  apps: null,
  inputs: null,
  flow: null,
  focus: null,
  monitor: { events: [], timer: null, since: 0 },
};

// ------------------------------------------------------------------ utils

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
  });
}

function keyName(code) { return KEYS[code] || ('Key ' + code); }
function keyLabel(code) { return KEYS[code] ? KEYS[code] + ' <small>(' + code + ')</small>' : 'Key ' + code; }

function actionText(a) {
  if (!a) return 'Normal behaviour';
  switch (a.type) {
    case 'disable': return 'Disabled';
    case 'pass': return 'Normal behaviour';
    case 'replace': return 'Acts as ' + keyName(a.to);
    case 'launch': return (a.input ? 'Switch to ' : 'Launch ') + (a.title || appTitle(a.app));
    case 'exec': return 'Run: ' + a.command;
  }
  return '?';
}

function appTitle(id) {
  var app = (S.apps || []).filter(function (a) { return a.id === id; })[0];
  return app ? app.title : id;
}

var toastTimer = null;
function toast(msg) {
  var el = document.getElementById('toast');
  el.textContent = msg;
  el.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function () { el.classList.remove('show'); }, 2600);
}

function setFocus(fid) { S.focus = fid; }

function daemonState() {
  var st = S.status;
  if (!st) return { cls: 'warn', text: 'Starting…' };
  if (!st.root) return { cls: 'bad', text: 'Service is not running as root', hint: 'Check that Homebrew Channel shows root status OK, then relaunch.' };
  if (!st.daemon || !st.daemon.running) return { cls: 'bad', text: 'Remapper is not running', hint: 'Try Settings → Remapper service → Restart.' };
  var ds = st.daemon.status || {};
  if (ds.config && ds.config.ok === false) return { cls: 'warn', text: 'Config error', hint: ds.config.error };
  var devs = (ds.devices || []).length;
  if (!devs) return { cls: 'warn', text: 'No remote device found', hint: 'The remapper found no LG remote input device to take over.' };
  return { cls: 'ok', text: 'Active' };
}

// ------------------------------------------------------------------ render

function render() {
  var app = document.getElementById('app');
  var html = '';
  switch (S.screen) {
    case 'boot': html = renderBoot(); break;
    case 'error': html = renderError(); break;
    case 'main': html = renderMain(); break;
    case 'flow': html = renderFlow(); break;
    case 'settings': html = renderSettings(); break;
    case 'monitor': html = renderMonitor(); break;
    case 'about': html = renderAbout(); break;
  }
  app.innerHTML = html;
  var target = S.focus && app.querySelector('[data-fid="' + S.focus + '"]');
  if (!(target && nav.focus(target))) nav.focusFirst();
}

function header(subtitle) {
  return '<div class="topbar"><div class="brand"><img src="icon.png" alt=""><div><h1>LG Input Mapper</h1>' +
    '<div class="sub">' + esc(subtitle || 'Remap the buttons on your remote') + '</div></div></div></div>';
}

// Shown only when something needs attention; nothing is drawn when all is well.
function problemBanner() {
  var ds = daemonState();
  if (ds.cls === 'ok') return '';
  return '<div class="error-box" style="margin-bottom:22px"><b>' + esc(ds.text) + '</b>' +
    (ds.hint ? ' — ' + esc(ds.hint) : '') + '</div>';
}

function renderBoot() {
  return '<div class="boot"><div class="brand"><img src="icon.png" alt="" style="width:96px;height:96px;border-radius:24px"></div>' +
    '<h1 style="margin:0">LG Input Mapper</h1></div>';
}

function renderError() {
  return header('Something went wrong') + '<div class="content"><div class="error-box"><h2 style="margin-top:0">' + esc(S.error.title) + '</h2>' +
    '<p>' + esc(S.error.message) + '</p>' + (S.error.detail ? '<p class="muted mon">' + esc(S.error.detail) + '</p>' : '') + '</div>' +
    '<div class="row" style="margin-top:24px"><button class="btn primary" data-fid="retry" data-autofocus data-act="retry">Try again</button>' +
    '<button class="btn" data-fid="exit" data-act="exit">Exit</button></div></div>';
}

function renderMain() {
  var maps = (S.config && S.config.mappings) || [];
  var html = header() + '<div class="content">' + problemBanner();
  html += '<div class="row"><button class="btn primary big" data-fid="remap" data-autofocus data-act="remap">' + ICON.plus + 'Remap a button</button>' +
    '<button class="btn big" data-fid="settings" data-act="settings">' + ICON.gear + 'Settings</button></div>';
  html += '<div class="section-title">Your mappings</div>';
  if (!maps.length) {
    html += '<div class="empty">No buttons remapped yet. Press <b>Remap a button</b> to start.</div>';
  } else {
    html += '<div class="stack">';
    maps.forEach(function (m, i) {
      html += '<button class="card' + (m.enabled === false ? ' off' : '') + '" data-fid="map-' + m.key + '" data-act="edit" data-idx="' + i + '">' +
        '<div class="key">' + keyLabel(m.key) + '</div><div class="arrow">→</div>' +
        '<div class="what">' + esc(actionText(m.action)) + (m.hold ? '<div class="hold">Held: ' + esc(actionText(m.hold)) + '</div>' : '') + '</div>' +
        (m.enabled === false ? '<span class="badge">Off</span>' : '') + '</button>';
    });
    html += '</div>';
  }
  return html + '</div>';
}

// --------------------------------------------------------------- flow steps

function renderFlow() {
  var f = S.flow;
  var body = '', title = '', lead = '', footer = '';
  switch (f.step) {
    case 'capture-source':
    case 'capture-target':
      title = f.step === 'capture-source' ? 'Press the button you want to remap' : 'Press the button it should act as';
      lead = f.step === 'capture-source' ? 'The button will not do its normal action while we listen. Press Back to cancel.' :
        'Any button works here, or choose one from the list. Press Back to go back.';
      body = renderCountdown(f);
      if (f.step === 'capture-target') footer = '<button class="btn" data-fid="pick-list" data-act="pick-list">Choose from a list instead</button>';
      break;
    case 'choose-action':
      var forHold = f.target === 'hold';
      title = forHold ? 'What should ' + keyName(f.key) + ' do when held?' : 'What should ' + keyName(f.key) + ' do?';
      lead = forHold ? 'Hold for about half a second to trigger this instead of the normal tap action.' : 'Pick what happens when you press it.';
      body = '<div class="list">' +
        item('act-disable', 'act', 'disable', ICON.block, 'Do nothing', 'Disable the button completely') +
        item('act-replace', 'act', 'replace', ICON.swap, 'Act as another button', 'Play/Pause, Mute, Info, numbers, colours, or any button') +
        item('act-launch', 'act', 'launch', ICON.play, 'Launch an app', 'Open any installed app') +
        item('act-input', 'act', 'input', ICON.hdmi, 'Switch to an input', 'HDMI, Live TV and other inputs on this TV') +
        item('act-exec', 'act', 'exec', ICON.term, 'Run a command', 'Shell command as root, with presets') +
        (forHold ? item('act-none', 'act', 'none', ICON.none, 'No hold action', 'Keep only the tap action')
                 : item('act-pass', 'act', 'pass', ICON.hold, 'Keep the normal press', 'Leave the button as it is and only add a hold action')) +
        '</div>';
      break;
    case 'pick-key':
      title = 'Choose a button';
      lead = keyName(f.key) + ' will act as the button you pick.';
      body = '<div class="list">' + TARGET_LIST.map(function (code) {
        return item('key-' + code, 'pick-key', code, '', keyName(code), 'code ' + code);
      }).join('') + '</div>';
      break;
    case 'pick-input':
      title = 'Choose an input';
      lead = keyName(f.key) + ' will switch to the input you pick.';
      if (!S.inputs) body = '<div class="empty">Loading inputs…</div>';
      else if (!S.inputs.length) body = '<div class="empty">No inputs reported by the TV.</div>';
      else body = '<div class="list">' + S.inputs.map(function (a) {
        return item('in-' + a.id, 'pick-input', a.id, ICON.hdmi, a.title, a.connected === false ? 'Nothing connected' : (a.deviceLabel || ''));
      }).join('') + '</div>';
      break;
    case 'pick-app':
      title = 'Choose an app';
      lead = keyName(f.key) + ' will launch the app you pick.';
      if (!S.apps) body = '<div class="empty">Loading apps…</div>';
      else body = '<div class="list">' + S.apps.map(function (a) {
        return item('app-' + a.id, 'pick-app', a.id, '', a.title, a.id);
      }).join('') + '</div>';
      break;
    case 'command':
      title = 'Run a command';
      lead = 'Runs as root when ' + keyName(f.key) + ' is ' + (f.target === 'hold' ? 'held' : 'pressed') + '. Pick a preset or type your own.';
      body = '<div class="field"><label>Command</label><input class="input" data-fid="cmd" data-autofocus id="cmd-input" type="text" value="' + esc(f.command || '') + '" placeholder="e.g. luna-send -n 1 luna://…"></div>' +
        '<div class="section-title">Presets</div><div class="list">' + COMMAND_PRESETS.map(function (p, i) {
          return item('preset-' + i, 'preset', i, '', p.title, p.desc);
        }).join('') + '</div>';
      footer = '<button class="btn primary" data-fid="cmd-ok" data-act="command-ok">Use this command</button>';
      break;
    case 'ask-hold':
      title = 'Do something different when held?';
      lead = 'A long press (about half a second) can trigger a second action.';
      body = '<div class="list">' +
        item('hold-no', 'hold', 'no', ICON.check, 'No, finish', 'Only the tap action') +
        item('hold-yes', 'hold', 'yes', ICON.hold, 'Yes, choose a hold action', 'Tap and hold do different things') + '</div>';
      break;
    case 'summary':
      title = 'Ready to save';
      body = '<div class="summary">' +
        '<div class="line"><div class="label">Button</div><div>' + keyLabel(f.key) + '</div></div>' +
        '<div class="line"><div class="label">Press</div><div>' + esc(actionText(f.action)) + '</div></div>' +
        (f.hold ? '<div class="line"><div class="label">Hold</div><div>' + esc(actionText(f.hold)) + '</div></div>' : '') + '</div>';
      footer = '<button class="btn" data-fid="cancel" data-act="cancel">Cancel</button><button class="btn primary" data-fid="save" data-autofocus data-act="save">Save</button>';
      break;
    case 'edit-menu':
      var m = f.mapping;
      title = keyLabel(m.key).replace(/<[^>]+>/g, '') ;
      lead = actionText(m.action) + (m.hold ? ' · held: ' + actionText(m.hold) : '');
      body = '<div class="list">' +
        item('e-change', 'edit', 'change', ICON.edit, 'Change what it does', 'Pick a new action') +
        item('e-hold', 'edit', 'hold', ICON.hold, m.hold ? 'Change the hold action' : 'Add a hold action', 'Different action for a long press') +
        item('e-toggle', 'edit', 'toggle', ICON.power, m.enabled === false ? 'Turn on' : 'Turn off', 'Keep the mapping but pause it') +
        item('e-remove', 'edit', 'remove', ICON.trash, 'Remove', 'Back to normal behaviour') + '</div>';
      break;
  }
  return header() + '<div class="flow" data-scope><h2>' + esc(title) + '</h2>' + (lead ? '<p class="lead">' + esc(lead) + '</p>' : '') +
    '<div class="body">' + body + '</div><div class="footer">' + footer + '</div></div>';
}

function item(fid, act, value, icon, title, desc) {
  return '<button class="item" data-fid="' + fid + '" data-act="' + act + '" data-value="' + esc(value) + '">' +
    (icon ? '<span class="icon">' + icon + '</span>' : '') + '<span class="title">' + esc(title) +
    (desc ? '<div class="desc">' + esc(desc) + '</div>' : '') + '</span></button>';
}

function renderCountdown(f) {
  var C = 2 * Math.PI * 118;
  var left = Math.max(0, f.deadline - Date.now());
  var frac = left / COUNTDOWN_MS;
  return '<div class="countdown"><div class="ring"><svg viewBox="0 0 260 260"><circle class="track" cx="130" cy="130" r="118" fill="none" stroke-width="14"/>' +
    '<circle class="bar" cx="130" cy="130" r="118" fill="none" stroke-width="14" stroke-linecap="round" stroke-dasharray="' + C + '" stroke-dashoffset="' + (C * (1 - frac)) + '"/></svg>' +
    '<div class="num" id="cd-num">' + Math.ceil(left / 1000) + '</div></div>' +
    '<div class="capture-msg" id="cd-msg">' + esc(f.message || '') + '</div>' +
    (f.pressed ? '<div class="pressed">' + esc(keyName(f.pressed)) + '</div>' : '') + '</div>';
}

// ---------------------------------------------------------------- settings

function renderSettings() {
  var st = S.status || {};
  var d = st.daemon || {};
  var auto = st.autostart || {};
  var holdMs = (S.config && S.config.holdMs) || 500;
  return header('Settings') + '<div class="content stack" data-scope>' +
    '<button class="setting" data-fid="s-auto" data-autofocus data-act="autostart"><span class="title">Start at boot<div class="desc">Apply your mappings automatically after the TV starts</div></span><span class="toggle' + (auto.enabled ? ' on' : '') + '"></span></button>' +
    '<button class="setting" data-fid="s-hold" data-act="hold-ms"><span class="title">Hold duration<div class="desc">How long a button must be held to count as a long press</div></span><span class="value">' + holdMs + ' ms</span></button>' +
    '<button class="setting" data-fid="s-daemon" data-act="restart"><span class="title">Remapper service<div class="desc">' + (d.running ? 'Running (pid ' + d.pid + ', v' + d.version + ')' : 'Not running') + ' · press to restart</div></span><span class="value">' + (d.running ? 'Restart' : 'Start') + '</span></button>' +
    '<button class="setting" data-fid="s-monitor" data-act="monitor"><span class="title">Key monitor<div class="desc">See the code of every button as you press it</div></span><span class="value">›</span></button>' +
    '<button class="setting" data-fid="s-reset" data-act="reset"><span class="title">Remove all mappings<div class="desc">Everything back to normal</div></span><span class="value" style="color:var(--bad)">Reset</span></button>' +
    '<button class="setting" data-fid="s-about" data-act="about"><span class="title">About<div class="desc">Version ' + esc(VERSION) + ' · credits</div></span><span class="value">›</span></button>' +
    '</div>';
}

function renderMonitor() {
  var evs = S.monitor.events.slice(-40); // oldest first; newest sits just above the Back button
  var html = header('Key monitor') + '<div class="content mon" data-scope>' +
    '<p class="muted" style="margin-top:0">Press any button. Remappable buttons are held back while this screen is open, so nothing launches. Back leaves this screen.</p>';
  if (!evs.length) html += '<div class="empty">Waiting for a button press…</div>';
  evs.forEach(function (e) {
    var v = e.value === 1 ? 'press' : e.value === 0 ? 'release' : 'repeat';
    var act = e.act === 'capture' ? 'held back' : e.act === 'capture:pass' ? 'passed through' : e.act + (e.to ? ' → ' + keyName(e.to) : '');
    html += '<div class="ev"><span class="t">' + new Date(e.t).toLocaleTimeString() + '</span><span class="k">' + keyLabel(e.code) + '</span>' +
      '<span class="v">' + v + '</span><span class="a">' + esc(act) + '</span></div>';
  });
  return html + '<button class="btn" data-fid="mon-back" data-autofocus data-act="back" style="margin-top:20px">Back</button></div>';
}

function renderAbout() {
  return header('About') + '<div class="content about" data-scope>' +
    '<p><b>LG Input Mapper ' + esc(VERSION) + '</b> — remap, disable or repurpose the buttons on your LG remote. Requires Homebrew Channel with root.</p>' +
    '<p>How it works: a small daemon (lginputmapperd) takes exclusive control of the remote\'s input device, applies your mappings, and re-emits the result into a sibling device the TV already listens to. No process injection, no hooks.</p>' +
    '<p class="section-title">Credits</p>' +
    '<p>Simon34545 — lginputhook (LG Input Hook), the original remapper this project succeeds; AkshayRao27 — its button ID list.</p>' +
    '<p>Andrew Fraley — magic_mapper, which showed that grabbing the remote\'s device and injecting into a sibling works (C5 codes by DanielPower).</p>' +
    '<p>Informatic, smx-smx, sundermann — prior art on the hook-based approach. webosbrew — Homebrew Channel.</p>' +
    '<p>Bundled: cJSON (Dave Gamble, MIT), webOSTV.js (LG Electronics, Apache-2.0), core-js (Denis Pushkarev, MIT).</p>' +
    '<button class="btn" data-fid="about-back" data-autofocus data-act="back" style="margin-top:20px">Back</button></div>';
}

// ---------------------------------------------------------------- actions

document.getElementById('app').addEventListener('click', function (ev) {
  var el = ev.target;
  while (el && el !== this && !el.getAttribute('data-act')) el = el.parentNode;
  if (!el || el === this) return;
  var act = el.getAttribute('data-act');
  var value = el.getAttribute('data-value');
  if (el.hasAttribute('data-fid')) setFocus(el.getAttribute('data-fid'));
  trace('click ' + act + (value ? ':' + value : '') + ' (' + (el.getAttribute('data-fid') || '') + ')');
  handle(act, value, el);
});

function handle(act, value, el) {
  var f = S.flow;
  switch (act) {
    case 'retry': boot(); break;
    case 'exit': exitApp(); break;
    case 'remap': startFlow({ step: 'capture-source', target: 'action' }); break;
    case 'settings': S.screen = 'settings'; setFocus('s-auto'); render(); break;
    case 'about': S.screen = 'about'; setFocus(null); render(); break;
    case 'monitor': openMonitor(); break;
    case 'back': goBack(); break;
    case 'edit':
      if (S.screen === 'main') {
        var m = S.config.mappings[parseInt(el.getAttribute('data-idx'), 10)];
        startFlow({ step: 'edit-menu', mapping: m, key: m.key, action: m.action, hold: m.hold, editing: true });
      } else editMenu(value);
      break;
    case 'pick-list': stopCountdown(); f.step = 'pick-key'; setFocus(null); render(); break;
    case 'act': chooseAction(value); break;
    case 'pick-key': applyAction({ type: 'replace', to: parseInt(value, 10) }); break;
    case 'pick-app': applyAction({ type: 'launch', app: value, title: appTitle(value) }); break;
    case 'pick-input':
      var inp = (S.inputs || []).filter(function (i) { return i.id === value; })[0];
      applyAction({ type: 'launch', app: inp.appId, title: inp.title, input: true });
      break;
    case 'preset':
      f.command = COMMAND_PRESETS[parseInt(value, 10)].command;
      document.getElementById('cmd-input').value = f.command;
      setFocus('cmd-ok'); render();
      break;
    case 'command-ok':
      var cmd = (document.getElementById('cmd-input').value || '').trim();
      if (!cmd) { toast('Enter a command first'); return; }
      applyAction({ type: 'exec', command: cmd });
      break;
    case 'hold':
      if (value === 'yes') { f.target = 'hold'; f.step = 'choose-action'; setFocus(null); render(); }
      else { f.step = 'summary'; setFocus('save'); render(); }
      break;
    case 'save': saveFlow(); break;
    case 'cancel': endFlow(); break;
    case 'autostart': toggleAutostart(); break;
    case 'hold-ms': cycleHoldMs(); break;
    case 'restart': restartDaemon(); break;
    case 'reset': resetAll(); break;
  }
}

function goBack() {
  if (S.screen === 'flow') {
    var f = S.flow;
    stopCountdown();
    if (f.step === 'capture-source' || f.step === 'edit-menu' || f.step === 'summary' && !f.editing) return endFlow();
    if (f.step === 'summary') return endFlow();
    if (f.step === 'choose-action') {
      if (f.editing) { f.step = 'edit-menu'; setFocus(null); return render(); }
      if (f.target === 'hold') { f.step = 'ask-hold'; setFocus(null); return render(); }
      return startFlow({ step: 'capture-source', target: 'action' });
    }
    if (f.step === 'capture-target' || f.step === 'pick-key' || f.step === 'pick-app' || f.step === 'pick-input' || f.step === 'command') { f.step = 'choose-action'; setFocus(null); return render(); }
    if (f.step === 'ask-hold') { f.target = 'action'; f.step = 'choose-action'; setFocus(null); return render(); }
    return endFlow();
  }
  if (S.screen === 'monitor') { closeMonitor(); S.screen = 'settings'; setFocus('s-monitor'); return render(); }
  if (S.screen === 'about') { S.screen = 'settings'; setFocus('s-about'); return render(); }
  if (S.screen === 'settings') { S.screen = 'main'; setFocus('settings'); return render(); }
  if (S.screen === 'main' || S.screen === 'error') return exitApp();
}

function exitApp() {
  if (window.webOS && webOS.platformBack) webOS.platformBack();
}

// ------------------------------------------------------------------- flow

function startFlow(flow) {
  S.flow = flow;
  S.screen = 'flow';
  setFocus(null);
  render();
  if (flow.step === 'capture-source') startCountdown();
}

function endFlow() {
  stopCountdown();
  S.flow = null;
  S.screen = 'main';
  setFocus('remap');
  render();
}

function editMenu(what) {
  var f = S.flow;
  if (what === 'change') { f.target = 'action'; f.step = 'choose-action'; setFocus(null); render(); }
  else if (what === 'hold') { f.target = 'hold'; f.step = 'choose-action'; setFocus(null); render(); }
  else if (what === 'toggle') {
    f.mapping.enabled = f.mapping.enabled === false;
    persist(S.config).then(function () { toast(f.mapping.enabled ? 'Mapping turned on' : 'Mapping turned off'); endFlow(); });
  } else if (what === 'remove') {
    S.config.mappings = S.config.mappings.filter(function (m) { return m !== f.mapping; });
    persist(S.config).then(function () { toast('Mapping removed'); endFlow(); });
  }
}

function chooseAction(type) {
  var f = S.flow;
  if (type === 'none') { f.hold = null; f.step = 'summary'; setFocus('save'); return render(); }
  if (type === 'pass') { f.action = { type: 'pass' }; f.target = 'hold'; f.step = 'choose-action'; setFocus(null); return render(); }
  if (type === 'disable') return applyAction({ type: 'disable' });
  if (type === 'replace') { f.step = 'capture-target'; setFocus(null); render(); return startCountdown(); }
  if (type === 'input') {
    f.step = 'pick-input'; setFocus(null); render();
    if (!S.inputs) api.call('listInputs').then(function (r) { S.inputs = r.inputs; if (S.flow && S.flow.step === 'pick-input') render(); }, function (e) { S.inputs = []; toast('Could not list inputs: ' + e.message); render(); });
    return;
  }
  if (type === 'launch') {
    f.step = 'pick-app'; setFocus(null); render();
    if (!S.apps) api.call('listApps').then(function (r) { S.apps = r.apps; if (S.flow && S.flow.step === 'pick-app') render(); }, function (e) { toast('Could not list apps: ' + e.message); });
    return;
  }
  if (type === 'exec') { f.command = (f.target === 'hold' ? f.hold : f.action) && (f.target === 'hold' ? f.hold : f.action).type === 'exec' ? (f.target === 'hold' ? f.hold : f.action).command : ''; f.step = 'command'; setFocus('cmd'); render(); }
}

function applyAction(action) {
  var f = S.flow;
  stopCountdown();
  if (f.target === 'hold') f.hold = action; else f.action = action;
  if (f.editing || f.target === 'hold') { f.step = 'summary'; setFocus('save'); }
  else { f.step = 'ask-hold'; setFocus('hold-no'); }
  render();
}

function saveFlow() {
  var f = S.flow;
  var cfg = S.config;
  var existing = cfg.mappings.filter(function (m) { return m.key === f.key; })[0];
  var m = existing || { key: f.key, enabled: true };
  m.label = keyName(f.key);
  m.action = f.action || { type: 'pass' };
  if (f.hold) m.hold = f.hold; else delete m.hold;
  if (!existing) cfg.mappings.push(m);
  persist(cfg).then(function () { toast('Saved: ' + keyName(f.key) + ' → ' + actionText(m.action)); endFlow(); });
}

function persist(cfg) {
  return api.call('setConfig', { config: cfg }).then(function (r) {
    S.config = r.config;
    return refreshStatus();
  }, function (e) {
    toast('Could not save: ' + e.message);
    // Local state was already changed: reload what the service actually has.
    return api.call('getConfig').then(function (r) { S.config = r.config; render(); throw e; }, function () { throw e; });
  });
}

// --------------------------------------------------------------- countdown

var cd = { timer: null, poll: null, active: false };

function startCountdown() {
  var f = S.flow;
  f.deadline = Date.now() + COUNTDOWN_MS;
  f.message = '';
  f.pressed = null;
  cd.active = true;
  var since = Date.now();
  var passthrough = f.step === 'capture-target' ? NAV_KEYS : [BACK];
  api.call('startCapture', { ms: COUNTDOWN_MS + 1500, swallow: true, passthrough: passthrough }).then(null, function (e) {
    f.message = 'Capture failed: ' + e.message;
  });
  render();
  cd.timer = setInterval(function () {
    var left = Math.max(0, f.deadline - Date.now());
    var num = document.getElementById('cd-num');
    if (num) num.textContent = Math.ceil(left / 1000);
    var bar = document.querySelector('.ring .bar');
    if (bar) bar.setAttribute('stroke-dashoffset', 2 * Math.PI * 118 * (1 - left / COUNTDOWN_MS));
    if (left <= 0) { stopCountdown(); toast('No button pressed'); if (f.step === 'capture-source') endFlow(); else { f.step = 'choose-action'; setFocus(null); render(); } }
  }, 100);
  cd.poll = setInterval(function () {
    api.call('getEvents', { since: since }).then(function (r) {
      if (!cd.active) return;
      var hit = null;
      r.events.forEach(function (e) {
        since = Math.max(since, e.t);
        if (e.type !== 1 || e.value !== 1 || NOISE[e.code] || e.code === BACK) return;
        if (f.step === 'capture-target' && NAV_KEYS.indexOf(e.code) >= 0) return;
        if (!hit) hit = e;
      });
      if (!hit) return;
      if (f.step === 'capture-source' && EXCLUDED[hit.code]) {
        f.message = keyName(hit.code) + ' can\'t be remapped. Try another button.';
        f.deadline = Date.now() + COUNTDOWN_MS;
        api.call('startCapture', { ms: COUNTDOWN_MS + 1500, swallow: true, passthrough: [BACK] }).then(null, function () {});
        var msg = document.getElementById('cd-msg');
        if (msg) msg.textContent = f.message;
        return;
      }
      stopCountdown();
      if (f.step === 'capture-source') {
        f.key = hit.code;
        var existing = S.config.mappings.filter(function (m) { return m.key === hit.code; })[0];
        if (existing) { startFlow({ step: 'edit-menu', mapping: existing, key: existing.key, action: existing.action, hold: existing.hold, editing: true }); return; }
        f.step = 'choose-action';
      } else {
        applyAction({ type: 'replace', to: hit.code });
        return;
      }
      setFocus(null);
      render();
    }, function () {});
  }, 250);
}

function stopCountdown() {
  if (!cd.active) return;
  cd.active = false;
  clearInterval(cd.timer); clearInterval(cd.poll);
  api.call('stopCapture', {}).then(null, function () {});
}

// ----------------------------------------------------------------- monitor

function monitorHold() {
  // Hold back every remappable button while the monitor is open; navigation,
  // Home, Settings, volume and channel keep working.
  api.call('startCapture', { ms: MONITOR_CAPTURE_MS, swallow: true, passthrough: EXCLUDED_LIST }).then(null, function () {});
}

function openMonitor() {
  S.screen = 'monitor';
  S.monitor.events = [];
  S.monitor.since = Date.now();
  setFocus('mon-back');
  render();
  monitorHold();
  S.monitor.hold = setInterval(monitorHold, MONITOR_CAPTURE_MS - 8000);
  S.monitor.timer = setInterval(function () {
    api.call('getEvents', { since: S.monitor.since }).then(function (r) {
      var fresh = r.events.filter(function (e) { return e.type === 1 && !NOISE[e.code]; });
      r.events.forEach(function (e) { S.monitor.since = Math.max(S.monitor.since, e.t); });
      if (fresh.length) {
        S.monitor.events = S.monitor.events.concat(fresh).slice(-200);
        if (S.screen === 'monitor') {
          render();
          var c = document.querySelector('.content');
          if (c) c.scrollTop = c.scrollHeight;
        }
      }
    }, function () {});
  }, 400);
}

function closeMonitor() {
  clearInterval(S.monitor.timer);
  clearInterval(S.monitor.hold);
  S.monitor.timer = null;
  S.monitor.hold = null;
  api.call('stopCapture', {}).then(null, function () {});
}

// ---------------------------------------------------------------- settings

function toggleAutostart() {
  var enable = !(S.status && S.status.autostart && S.status.autostart.enabled);
  api.call('autostart', { enable: enable }).then(function () {
    toast(enable ? 'Will start at boot' : 'Will not start at boot');
    return refreshStatus();
  }, function (e) { toast(e.message); }).then(render);
}

function cycleHoldMs() {
  var opts = [400, 500, 600, 800, 1000];
  var cur = S.config.holdMs || 500;
  var next = opts[(opts.indexOf(cur) + 1) % opts.length];
  S.config.holdMs = next;
  persist(S.config).then(render);
}

function restartDaemon() {
  toast('Restarting…');
  api.call('daemon', { action: 'restart' }).then(function () { return refreshStatus(); }).then(function () { toast('Remapper service restarted'); render(); }, function (e) { toast(e.message); });
}

function resetAll() {
  if (!S.config.mappings.length) { toast('Nothing to remove'); return; }
  S.config.mappings = [];
  persist(S.config).then(function () { toast('All mappings removed'); render(); });
}

function refreshStatus() {
  return api.call('status').then(function (st) { S.status = st; return st; });
}

// -------------------------------------------------------------------- boot

function bootLog(line) { S.bootLog.push(line); }

function fail(title, message, detail) {
  S.screen = 'error';
  S.error = { title: title, message: message, detail: detail };
  setFocus('retry');
  render();
}

function retry(fn, times, delay) {
  return fn().then(null, function (e) {
    if (times <= 0) throw e;
    return new Promise(function (r) { setTimeout(r, delay); }).then(function () { return retry(fn, times - 1, delay); });
  });
}

var bootAttempt = 0;

function boot() {
  S.screen = 'boot';
  S.bootLog = [];
  render();
  bootAttempt++;
  bootLog('Checking Homebrew Channel…');
  api.hbchannel('getConfiguration', {}).then(function (cfg) {
    if (!cfg.root) throw new Error('Homebrew Channel is not rooted. Root status must be OK in its settings.');
    bootLog('Elevating service…');
    return api.hbchannel('exec', { command: '/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service com.lginputmapper.app.service' });
  }).then(function () {
    bootLog('Starting remapper…');
    return retry(function () { return api.call('setup'); }, 6, 1000);
  }).then(function () {
    // setup locks the service's bus role down to this app. Should that keep us
    // out on this TV, the service restores the role after a few seconds.
    return retry(function () { return Promise.all([api.call('getConfig'), refreshStatus()]); }, 5, 2500);
  }).then(function (res) {
    S.config = res[0].config;
    S.screen = 'main';
    setFocus('remap');
    render();
    // Warm the app list in the background for the launch picker.
    api.call('listApps').then(function (r) { S.apps = r.apps; if (S.screen === 'main') render(); }, function () {});
  }, function (e) {
    // Right after boot, or while an update is being installed, the helpers can
    // be briefly unavailable: retry quietly a few times before giving up.
    var transient = e.code === 'timeout' || e.code === 'not_root' || /not (running|available)|timed? ?out|No answer/i.test(e.message || '');
    if (transient && bootAttempt < 3) {
      setTimeout(boot, 3000);
      return;
    }
    bootAttempt = 0;
    fail('Could not start', e.message, 'after: ' + S.bootLog[S.bootLog.length - 1] + (e.code ? ' · code: ' + e.code : ''));
  });
}

window.onerror = function (msg, src, line, col) {
  try {
    document.getElementById('app').innerHTML = '<div class="content"><div class="error-box"><h2 style="margin-top:0">Script error</h2>' +
      '<p>' + esc(msg) + '</p><p class="muted">' + esc((src || '').split('/').pop() + ':' + line + ':' + col) + '</p></div></div>';
  } catch (e) {}
};

function trace(msg) {
  api.call('clientLog', { msg: S.screen + (S.flow ? '/' + S.flow.step : '') + ': ' + msg }).then(null, function () {});
}

nav.install({ back: goBack, submit: function (el) { if (el.id === 'cmd-input') handle('command-ok'); }, trace: trace });
boot();

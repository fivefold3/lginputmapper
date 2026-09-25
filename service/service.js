/*
 * LG Input Mapper service.
 *
 * Runs as root (after Homebrew Channel elevates it) and is responsible for:
 *  - keeping the lginputmapperd daemon running and installing its boot script,
 *  - reading/writing the mapping config the daemon watches,
 *  - exposing status, the live key event log and helper calls (app list,
 *    capture mode) over Luna to the TV app.
 */
var Service = require('webos-service');
var fs = require('fs');
var path = require('path');
var childProcess = require('child_process');
var pkgInfo = require('./package.json');

var APP_VERSION = typeof __APP_VERSION__ !== 'undefined' ? __APP_VERSION__ : pkgInfo.version;
var APP_ID = 'com.lginputmapper.app';
var SERVICE_DIR = fs.existsSync(path.join(__dirname, 'bin')) ? __dirname : process.cwd();
var CONFIG_DIR = '/home/root/.config/lginputmapper';
var CONFIG_PATH = CONFIG_DIR + '/config.json';
// Earlier installs (LG Input Hook 1.x keybinds and the 2.0 pre-release) are imported once.
var LEGACY_DIR = '/home/root/.config/lginputhook';
var LEGACY_PATH = LEGACY_DIR + '/keybinds.json';
var PRERELEASE_PATHS = ['/home/root/.config/magicremap/config.json', LEGACY_DIR + '/config.json'];
var STATE_DIR = '/tmp/lginputmapperd';
var INIT_DIR = '/var/lib/webosbrew/init.d';
var INIT_SCRIPT = INIT_DIR + '/lginputmapper';
var HBCHANNEL_SERVICE = '/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service';
// Installed package trees (ares-package ships every directory as 0777).
var INSTALL_DIRS = [
  '/media/developer/apps/usr/palm/applications/' + APP_ID,
  '/media/developer/apps/usr/palm/services/' + pkgInfo.name,
  '/media/developer/apps/usr/palm/packages/' + APP_ID,
];
// Homebrew Channel's elevate-service opens our LS2 role to everyone; see lockRole().
var LUNA_ROOTS = ['/var/luna-service2-dev', '/var/luna-service2'];
var ELEVATE_NAMES = ['*', 'com.webos.service.capture.client*'];
// Written when locking the role cut the app off, so this version stops trying.
var ROLE_LOCK_OPTOUT = CONFIG_DIR + '/role-lock-disabled';

var service = new Service(pkgInfo.name);
var isRoot = typeof process.getuid === 'function' && process.getuid() === 0;

// The state dir sits in world-writable /tmp and holds the pid the service
// signals, the capture file the daemon obeys and the logs. Anything there we
// did not create ourselves (a symlink, or a directory owned by someone else,
// e.g. by the unprivileged service instance before elevation) is moved aside.
var stateDirOk = false;

function ensureStateDir() {
  if (stateDirOk && fs.existsSync(STATE_DIR)) return;
  if (!isRoot) {
    if (!fs.existsSync(STATE_DIR)) fs.mkdirSync(STATE_DIR, 448 /* 0700 */);
    return;
  }
  var st = null;
  try { st = fs.lstatSync(STATE_DIR); } catch (e) {}
  if (st && (!st.isDirectory() || st.uid !== 0)) {
    fs.renameSync(STATE_DIR, STATE_DIR + '.untrusted-' + Date.now());
    st = null;
  }
  if (!st) fs.mkdirSync(STATE_DIR, 448);
  fs.chmodSync(STATE_DIR, 448);
  stateDirOk = true;
}

function log() {
  var args = Array.prototype.slice.call(arguments);
  var line = new Date().toISOString() + ' ' + args.join(' ');
  console.log('[lginputmapper] ' + line);
  try {
    ensureStateDir();
    var file = STATE_DIR + '/service.log';
    try { if (fs.statSync(file).size > 512 * 1024) fs.renameSync(file, file + '.1'); } catch (e) {}
    fs.appendFileSync(file, line + '\n', { mode: 384 /* 0600 */ });
  } catch (e) {}
}

process.on('uncaughtException', function (err) {
  log('uncaught: ' + (err && err.stack || err));
});

// ----------------------------------------------------------------- helpers

function readJson(file, fallback) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (e) {
    return fallback;
  }
}

function mkdirp(dir, mode) {
  if (fs.existsSync(dir)) return;
  mkdirp(path.dirname(dir));
  fs.mkdirSync(dir, mode);
}

// Everything the service writes is only ever read by root (itself or the daemon).
function writeFileAtomic(file, data) {
  mkdirp(path.dirname(file), 448 /* 0700 */);
  var tmp = file + '.tmp';
  try { fs.unlinkSync(tmp); } catch (e) {}
  fs.writeFileSync(tmp, data, { mode: 384 /* 0600 */ });
  fs.renameSync(tmp, file);
}

function pidAlive(pid) {
  if (!pid) return false;
  try {
    process.kill(pid, 0);
    return true;
  } catch (e) {
    return e.code === 'EPERM';
  }
}

function lunaCall(uri, params) {
  return new Promise(function (resolve, reject) {
    service.call(uri, params || {}, function (message) {
      var payload = message.payload || {};
      if (payload.returnValue === false) {
        reject(new Error(payload.errorText || ('luna call failed: ' + uri)));
      } else {
        resolve(payload);
      }
    });
  });
}

function ApiError(message, code) {
  this.message = message;
  this.code = code || 'error';
}

function requireRoot() {
  if (!isRoot) throw new ApiError('Service is not running as root. Launch LG Input Mapper once so Homebrew Channel can elevate it.', 'not_root');
}

// ------------------------------------------------------------------ daemon

function daemonBinary() {
  var arch = process.arch === 'arm64' ? 'aarch64' : 'arm';
  return path.join(SERVICE_DIR, 'bin', 'lginputmapperd-' + arch);
}

function daemonStatus() {
  var status = readJson(STATE_DIR + '/status.json', null);
  var running = !!(status && pidAlive(status.pid));
  return {
    running: running,
    pid: running ? status.pid : null,
    version: running ? status.version : null,
    bundledVersion: APP_VERSION,
    status: running ? status : null,
    binary: daemonBinary(),
    binaryExists: fs.existsSync(daemonBinary()),
  };
}

function startDaemon(replace) {
  requireRoot();
  var bin = daemonBinary();
  if (!fs.existsSync(bin)) throw new ApiError('Daemon binary missing: ' + bin);
  try { fs.chmodSync(bin, 493 /* 0755 */); } catch (e) {}
  ensureStateDir();
  var args = ['--config', CONFIG_PATH, '--state-dir', STATE_DIR, '--log', STATE_DIR + '/daemon.log', '--pidfile', STATE_DIR + '/pid'];
  if (replace) args.push('--replace');
  var child = childProcess.spawn(bin, args, { detached: true, stdio: 'ignore' });
  child.on('error', function (err) { log('daemon spawn failed: ' + err); });
  child.unref();
  log('started daemon ' + bin + (replace ? ' (replace)' : ''));
}

function stopDaemon() {
  requireRoot();
  var st = daemonStatus();
  if (st.running) {
    try { process.kill(st.pid, 'SIGTERM'); } catch (e) {}
  }
}

function waitFor(check, timeoutMs) {
  return new Promise(function (resolve) {
    var started = Date.now();
    (function poll() {
      if (check() || Date.now() - started > timeoutMs) return resolve(check());
      setTimeout(poll, 150);
    })();
  });
}

var daemonStarting = null;

function ensureDaemon() {
  if (daemonStarting) return daemonStarting;
  var st = daemonStatus();
  if (st.running && st.version === APP_VERSION) return Promise.resolve(st);
  startDaemon(true);
  daemonStarting = waitFor(function () {
    var s = daemonStatus();
    return s.running && s.version === APP_VERSION;
  }, 4000).then(function () {
    daemonStarting = null;
    return daemonStatus();
  }, function (e) {
    daemonStarting = null;
    throw e;
  });
  return daemonStarting;
}

// ---------------------------------------------------------------- autostart

function initScriptContents() {
  return [
    '#!/bin/sh',
    '# Autostart for LG Input Mapper (' + APP_ID + '). Homebrew Channel runs the',
    '# scripts in ' + INIT_DIR + ' as root at boot. Removes itself if the app is gone.',
    'BIN="' + daemonBinary() + '"',
    'if [ ! -x "$BIN" ]; then rm -f "$0"; exit 0; fi',
    'mkdir -p -m 700 ' + STATE_DIR,
    'nohup "$BIN" --replace --config ' + CONFIG_PATH + ' --state-dir ' + STATE_DIR +
      ' --log ' + STATE_DIR + '/daemon.log --pidfile ' + STATE_DIR + '/pid >/dev/null 2>&1 </dev/null &',
    'exit 0',
    '',
  ].join('\n');
}

function autostartStatus() {
  var exists = fs.existsSync(INIT_SCRIPT);
  var current = exists && fs.readFileSync(INIT_SCRIPT, 'utf8') === initScriptContents();
  return { enabled: exists, upToDate: current, path: INIT_SCRIPT };
}

function setAutostart(enable) {
  requireRoot();
  if (enable) {
    mkdirp(INIT_DIR);
    fs.writeFileSync(INIT_SCRIPT, initScriptContents());
    fs.chmodSync(INIT_SCRIPT, 493);
  } else if (fs.existsSync(INIT_SCRIPT)) {
    fs.unlinkSync(INIT_SCRIPT);
  }
  return autostartStatus();
}

// ---------------------------------------------------------------- hardening

// The daemon binary, the init script target and the app's own JS (which may
// call this service) all live in the installed trees: nobody but their owner
// may write there. Directories and executables 0755, everything else 0644.
function lockDownTree(dir, owners) {
  var st;
  try { st = fs.lstatSync(dir); } catch (e) { return; }
  if (st.isSymbolicLink()) return;
  owners[st.uid + ':' + st.gid] = true;
  var mode = st.isDirectory() || (st.mode & 73 /* 0111 */) ? 493 /* 0755 */ : 420 /* 0644 */;
  if ((st.mode & 4095) !== mode) fs.chmodSync(dir, mode);
  if (st.isDirectory()) fs.readdirSync(dir).forEach(function (name) { lockDownTree(path.join(dir, name), owners); });
}

function lockDownInstall() {
  var owners = {};
  INSTALL_DIRS.forEach(function (dir) {
    try { lockDownTree(dir, owners); } catch (e) { log('lock down ' + dir + ': ' + e.message); }
  });
  return Object.keys(owners);
}

function roleLockOptedOut() {
  try { return fs.readFileSync(ROLE_LOCK_OPTOUT, 'utf8').trim() === APP_VERSION; } catch (e) { return false; }
}

function rescanServices() {
  return new Promise(function (resolve) {
    childProcess.execFile('ls-control', ['scan-services'], { timeout: 10000, env: { PATH: '/usr/sbin:/usr/bin:/sbin:/bin' } }, function (err) {
      if (err) log('ls-control scan-services failed: ' + err);
      resolve();
    });
  });
}

function writeRoleFile(file, text) {
  writeFileAtomic(file, text);
  fs.chmodSync(file, 420 /* 0644, the hub reads it */);
}

// Set while a freshly locked role waits for proof that the app still gets through.
var roleLockPending = null;
var ROLE_LOCK_CONFIRM_MS = 8000;

// Undo what elevate-service adds to our role (any name, any caller) and only
// let the app call in. The app runs elevate-service on every launch, which
// reopens the role, and calls setup right after, which locks it again.
// Should the hub then keep the app out on some TV (it names web app callers
// differently), no call arrives and revertRoleLock() restores the role and
// stops locking it for this version. Resolves to true if a role file changed.
function lockRole() {
  var originals = {};
  LUNA_ROOTS.forEach(function (root) {
    var file = root + '/roles.d/' + pkgInfo.name + '.service.json';
    var text;
    try { text = fs.readFileSync(file, 'utf8'); } catch (e) { return; }
    var role = JSON.parse(text);
    if (!role || !Array.isArray(role.allowedNames) || !Array.isArray(role.permissions)) return;
    var before = JSON.stringify(role);
    role.allowedNames = role.allowedNames.filter(function (n) { return ELEVATE_NAMES.indexOf(n) < 0; });
    role.permissions = role.permissions.filter(function (p) { return p && ELEVATE_NAMES.indexOf(p.service) < 0; });
    role.permissions.forEach(function (p) { p.inbound = [APP_ID]; });
    var after = JSON.stringify(role);
    if (after === before) return;
    log('locking role ' + file + ': ' + before + ' -> ' + after);
    writeRoleFile(file, after);
    originals[file] = text;
  });
  if (!Object.keys(originals).length) return Promise.resolve(false);
  return rescanServices().then(function () {
    if (roleLockPending) clearTimeout(roleLockPending.timer);
    roleLockPending = { originals: originals, timer: setTimeout(revertRoleLock, ROLE_LOCK_CONFIRM_MS) };
    return true;
  });
}

function confirmRoleLock() {
  if (!roleLockPending) return;
  clearTimeout(roleLockPending.timer);
  roleLockPending = null;
  log('role lock confirmed, the app still gets through');
}

function revertRoleLock() {
  var pending = roleLockPending;
  roleLockPending = null;
  if (!pending) return;
  log('no call from the app within ' + ROLE_LOCK_CONFIRM_MS + ' ms of locking the role: restoring it, not locking again in ' + APP_VERSION);
  try {
    Object.keys(pending.originals).forEach(function (file) { writeRoleFile(file, pending.originals[file]); });
    writeFileAtomic(ROLE_LOCK_OPTOUT, APP_VERSION + '\n');
  } catch (e) {
    log('restoring role: ' + e.message);
  }
  rescanServices();
}

function harden() {
  requireRoot();
  var result = { owners: lockDownInstall(), roleChanged: false, roleLockDisabled: roleLockOptedOut() };
  if (result.roleLockDisabled) return Promise.resolve(result);
  var locking;
  try { locking = lockRole(); } catch (e) { log('lock role: ' + e.message); locking = Promise.resolve(false); }
  return locking.then(function (changed) {
    result.roleChanged = changed;
    return result;
  });
}

// ------------------------------------------------------------------ config

function defaultConfig() {
  return {
    version: 2,
    holdMs: 500,
    devices: { include: ['LGE *'], exclude: [] },
    output: { mode: 'auto', device: 'LGE M-RCU - Builtin [2]' },
    mappings: [],
  };
}

function migrateLegacy(legacy) {
  var mappings = [];
  Object.keys(legacy).forEach(function (code) {
    var key = parseInt(code, 10);
    var old = legacy[code] || {};
    if (!key) return;
    var action;
    if (old.action === 'replace') action = { type: 'replace', to: parseInt(old.keycode, 10) || 0 };
    else if (old.action === 'exec') action = { type: 'exec', command: String(old.command || '') };
    else if (old.action === 'launch') action = { type: 'launch', app: String(old.id || '') };
    else if (old.action === 'ignore') action = { type: 'disable' };
    else return;
    mappings.push({ key: key, enabled: true, action: action });
  });
  return mappings;
}

function loadConfig() {
  var cfg = readJson(CONFIG_PATH, null);
  if (cfg) return cfg;
  if (fs.existsSync(CONFIG_PATH)) {
    // Exists but unreadable/invalid: leave it alone so nothing is lost; the
    // daemon keeps its last good mappings and reports the error in status.
    throw new ApiError('config.json is not valid JSON; fix or delete ' + CONFIG_PATH, 'config_invalid');
  }
  cfg = defaultConfig();
  var prePath = PRERELEASE_PATHS.filter(function (f) { return fs.existsSync(f); })[0];
  var pre = prePath ? readJson(prePath, null) : null;
  var legacy = readJson(LEGACY_PATH, null);
  if (pre && Array.isArray(pre.mappings)) {
    cfg = pre;
    log('imported pre-release config with ' + pre.mappings.length + ' mapping(s)');
  } else if (legacy && typeof legacy === 'object') {
    cfg.mappings = migrateLegacy(legacy);
    if (cfg.mappings.length) log('migrated ' + cfg.mappings.length + ' legacy keybinds');
  }
  if (isRoot) {
    try {
      saveConfig(cfg);
      if (pre) fs.renameSync(prePath, prePath + '.migrated');
      if (legacy) fs.renameSync(LEGACY_PATH, LEGACY_PATH + '.migrated');
    } catch (e) { log('cannot write initial config: ' + e); }
  }
  return cfg;
}

function validateConfig(cfg) {
  if (!cfg || typeof cfg !== 'object') throw new ApiError('config must be an object');
  if (!Array.isArray(cfg.mappings)) throw new ApiError('config.mappings must be an array');
  var seen = {};
  cfg.mappings.forEach(function (m, i) {
    var n = i + 1;
    if (!m || typeof m !== 'object') throw new ApiError('mapping #' + n + ' is invalid');
    if (!(m.key > 0 && m.key < 4096)) throw new ApiError('mapping #' + n + ' has an invalid key code');
    if (seen[m.key]) throw new ApiError('key ' + m.key + ' is mapped twice');
    seen[m.key] = true;
    [m.action, m.hold].forEach(function (a, ai) {
      if (!a) { if (ai === 0) throw new ApiError('mapping #' + n + ' has no action'); return; }
      var t = a.type;
      if (['pass', 'disable', 'replace', 'exec', 'launch'].indexOf(t) < 0) throw new ApiError('mapping #' + n + ': unknown action type');
      if (t === 'replace' && !(a.to > 0 && a.to < 4096)) throw new ApiError('mapping #' + n + ': replace needs a target key');
      if (t === 'exec' && !(a.command && String(a.command).trim())) throw new ApiError('mapping #' + n + ': command is empty');
      if (t === 'launch' && !a.app) throw new ApiError('mapping #' + n + ': no app selected');
    });
  });
}

function saveConfig(cfg) {
  validateConfig(cfg);
  cfg.version = 2;
  writeFileAtomic(CONFIG_PATH, JSON.stringify(cfg, null, 2) + '\n');
  return cfg;
}

// ------------------------------------------------------------------ events

// The daemon only writes events.jsonl while capture mode is on (key picker,
// key monitor) and deletes it when capture ends; nothing is kept otherwise.
var eventBuffer = [];
var eventOffset = 0;
var eventPartial = '';
var eventIno = 0;
var EVENT_BUFFER_MAX = 600;

function resetEvents() {
  eventBuffer = [];
  eventOffset = 0;
  eventPartial = '';
  eventIno = 0;
}

function pumpEvents() {
  var file = STATE_DIR + '/events.jsonl';
  var st;
  try { st = fs.statSync(file); } catch (e) { resetEvents(); return; }
  if (st.ino !== eventIno || st.size < eventOffset) { eventOffset = 0; eventPartial = ''; eventIno = st.ino; }
  if (st.size === eventOffset) return;
  var fd = fs.openSync(file, 'r');
  try {
    var len = Math.min(st.size - eventOffset, 256 * 1024);
    var buf = Buffer.alloc(len);
    var n = fs.readSync(fd, buf, 0, len, eventOffset);
    eventOffset += n;
    var text = eventPartial + buf.toString('utf8', 0, n);
    var lines = text.split('\n');
    eventPartial = lines.pop();
    lines.forEach(function (line) {
      if (!line) return;
      try { eventBuffer.push(JSON.parse(line)); } catch (e) {}
    });
    if (eventBuffer.length > EVENT_BUFFER_MAX) eventBuffer.splice(0, eventBuffer.length - EVENT_BUFFER_MAX);
  } finally {
    fs.closeSync(fd);
  }
}

// ------------------------------------------------------------------- API

var api = {
  status: function () {
    var d = daemonStatus();
    return {
      version: APP_VERSION,
      root: isRoot,
      arch: process.arch,
      node: process.version,
      daemon: d,
      autostart: autostartStatus(),
      configPath: CONFIG_PATH,
      legacyConfig: fs.existsSync(LEGACY_PATH),
    };
  },

  getConfig: function () {
    return { config: loadConfig() };
  },

  setConfig: function (params) {
    requireRoot();
    return { config: saveConfig(params.config) };
  },

  getEvents: function (params) {
    pumpEvents();
    var since = params.since || 0;
    var limit = params.limit || 200;
    var out = [];
    for (var i = eventBuffer.length - 1; i >= 0 && out.length < limit; i--) {
      if (eventBuffer[i].t > since) out.unshift(eventBuffer[i]);
      else break;
    }
    return { events: out, now: Date.now() };
  },

  startCapture: function (params) {
    requireRoot();
    var ms = Math.min(Math.max(params.ms || 8000, 1000), 120000);
    var until = Date.now() + ms;
    ensureStateDir();
    // Back (412) is always let through so the app can cancel the capture.
    var passthrough = Array.isArray(params.passthrough) ? params.passthrough : [412];
    writeFileAtomic(STATE_DIR + '/capture.json', JSON.stringify({ until: until, swallow: params.swallow !== false, passthrough: passthrough }));
    return { until: until };
  },

  stopCapture: function () {
    requireRoot();
    ensureStateDir();
    writeFileAtomic(STATE_DIR + '/capture.json', JSON.stringify({ until: 0 }));
    resetEvents();
    return {};
  },

  listApps: function () {
    return lunaCall('luna://com.webos.applicationManager/listApps', {}).then(function (res) {
      var apps = (res.apps || []).filter(function (a) {
        return a && a.id && a.title && a.visible !== false;
      }).map(function (a) {
        return { id: a.id, title: a.title, type: a.type };
      });
      apps.sort(function (a, b) { return a.title.localeCompare(b.title); });
      return { apps: apps };
    });
  },

  // External inputs (HDMI etc.) as launchable targets. Tries the external
  // input manager first, falls back to the well-known input app ids.
  listInputs: function () {
    var fallback = [
      { id: 'HDMI_1', title: 'HDMI 1', appId: 'com.webos.app.hdmi1' },
      { id: 'HDMI_2', title: 'HDMI 2', appId: 'com.webos.app.hdmi2' },
      { id: 'HDMI_3', title: 'HDMI 3', appId: 'com.webos.app.hdmi3' },
      { id: 'HDMI_4', title: 'HDMI 4', appId: 'com.webos.app.hdmi4' },
      { id: 'LIVETV', title: 'Live TV', appId: 'com.webos.app.livetv' },
    ];
    return lunaCall('luna://com.webos.service.eim/getAllInputStatus', {}).then(function (res) {
      var devices = res.devices || res.inputs || [];
      // MVPD_* entries are streaming apps the TV lists as pseudo-inputs; they live in the app picker.
      var inputs = devices.filter(function (d) { return d && d.appId && !/^MVPD/.test(d.id || ''); }).map(function (d) {
        return {
          id: d.id || d.appId,
          title: d.label || d.id || d.appId,
          deviceLabel: d.customLabel || d.deviceLabel || '',
          appId: d.appId,
          connected: d.connected,
        };
      });
      if (!inputs.length) throw new Error('empty');
      if (!inputs.some(function (i) { return i.appId === 'com.webos.app.livetv'; })) inputs.push(fallback[4]);
      return { inputs: inputs, source: 'eim' };
    }, function () {
      return { inputs: fallback, source: 'fallback' };
    });
  },

  daemon: function (params) {
    requireRoot();
    var action = params.action;
    if (action === 'stop') {
      stopDaemon();
      return waitFor(function () { return !daemonStatus().running; }, 3000).then(function () { return { daemon: daemonStatus() }; });
    }
    if (action === 'start' || action === 'restart') {
      startDaemon(true);
      return waitFor(function () { return daemonStatus().running; }, 3000).then(function () { return { daemon: daemonStatus() }; });
    }
    throw new ApiError('unknown daemon action');
  },

  autostart: function (params) {
    if (typeof params.enable === 'boolean') return { autostart: setAutostart(params.enable) };
    return { autostart: autostartStatus() };
  },

  clientLog: function (params) {
    log('app: ' + String(params.msg || '').slice(0, 300));
    return {};
  },

  getLog: function (params) {
    var file = STATE_DIR + (params.name === 'service' ? '/service.log' : '/daemon.log');
    try {
      var text = fs.readFileSync(file, 'utf8');
      if (text.length > 64 * 1024) text = text.slice(-64 * 1024);
      return { text: text };
    } catch (e) {
      return { text: '' };
    }
  },

  importLegacy: function () {
    requireRoot();
    var legacy = readJson(LEGACY_PATH, null);
    if (!legacy) throw new ApiError('no legacy keybinds.json found');
    var cfg = loadConfig();
    var existing = {};
    cfg.mappings.forEach(function (m) { existing[m.key] = true; });
    var added = migrateLegacy(legacy).filter(function (m) { return !existing[m.key]; });
    cfg.mappings = cfg.mappings.concat(added);
    saveConfig(cfg);
    fs.renameSync(LEGACY_PATH, LEGACY_PATH + '.migrated');
    return { added: added.length, config: cfg };
  },

  // Convenience for the TV app: autostart + daemon + hardening in one go.
  setup: function () {
    requireRoot();
    try { loadConfig(); } catch (e) { log('config: ' + e.message); }
    setAutostart(true);
    return ensureDaemon().then(function (d) {
      return harden().then(function (h) {
        return { daemon: d, autostart: autostartStatus(), hardening: h };
      });
    });
  },
};

function dispatch(method, params) {
  return new Promise(function (resolve) {
    if (!api[method]) throw new ApiError('unknown method ' + method, 'unknown_method');
    resolve(api[method](params || {}));
  });
}

// Only our own app may call in: setConfig stores commands the daemon runs as
// root. webos-service sets sender to the WAM application id of a web app
// caller (pid stripped), otherwise to the caller's bus name, both vouched for
// by the hub. This holds even while Homebrew Channel has the role wide open.
function callerOf(message) {
  if (message.sender) return String(message.sender);
  var m = message.ls2Message;
  try {
    var id = m.applicationID ? m.applicationID() : '';
    return id ? String(id).split(' ')[0] : String(m.senderServiceName() || '');
  } catch (e) {
    return '';
  }
}

Object.keys(api).forEach(function (method) {
  service.register(method, function (message) {
    var caller = callerOf(message);
    if (caller !== APP_ID) {
      log('refused ' + method + ' from ' + (caller || '(unknown caller)'));
      message.respond({ returnValue: false, errorText: 'Only ' + APP_ID + ' may call this service', errorCode: 'forbidden' });
      return;
    }
    confirmRoleLock();
    if (method !== 'getEvents' && method !== 'clientLog') log('call ' + method);
    dispatch(method, message.payload).then(function (res) {
      res = res || {};
      res.returnValue = true;
      message.respond(res);
    }, function (err) {
      log(method + ' failed: ' + (err && err.stack || err));
      message.respond({ returnValue: false, errorText: String(err && err.message || err), errorCode: err && err.code || 'error' });
    });
  });
});

// ---------------------------------------------------------------- startup

log('service ' + APP_VERSION + ' starting, root=' + isRoot + ', arch=' + process.arch + ', node=' + process.version + ', dir=' + SERVICE_DIR);

// Keep the process alive. Node exits as soon as its event loop is empty, and a
// registered bus handle alone does not hold it open; without a timer the
// service quit right after startup whenever the daemon was already running
// (the wait for a freshly spawned daemon used to mask this). The interval also
// restarts the daemon if it ever dies.
setInterval(function () {
  if (!isRoot) return;
  try {
    if (!daemonStatus().running) {
      log('daemon not running, restarting');
      ensureDaemon().then(null, function (e) { log('daemon restart failed: ' + e); });
    }
  } catch (e) {}
}, 30000);
try {
  service.activityManager.create('keepAlive', function () {});
} catch (e) {
  log('activityManager unavailable: ' + e);
}

if (isRoot) {
  try {
    // An install or update ships the trees world-writable again.
    log('install tree owners: ' + lockDownInstall().join(', '));
    // Created 0755/0644 by older versions.
    try { fs.chmodSync(CONFIG_DIR, 448 /* 0700 */); fs.chmodSync(CONFIG_PATH, 384 /* 0600 */); } catch (e) {}
    try { loadConfig(); } catch (e) { log('config: ' + e.message); }
    ensureDaemon().then(function (d) {
      log('daemon ' + (d.running ? 'running pid ' + d.pid : 'NOT running'));
    });
  } catch (e) {
    log('startup: ' + (e && e.stack || e));
  }
}

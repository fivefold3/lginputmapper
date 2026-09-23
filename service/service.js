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

var service = new Service(pkgInfo.name);
var isRoot = typeof process.getuid === 'function' && process.getuid() === 0;

function log() {
  var args = Array.prototype.slice.call(arguments);
  var line = new Date().toISOString() + ' ' + args.join(' ');
  console.log('[lginputmapper] ' + line);
  try {
    if (!fs.existsSync(STATE_DIR)) fs.mkdirSync(STATE_DIR);
    var file = STATE_DIR + '/service.log';
    try { if (fs.statSync(file).size > 512 * 1024) fs.renameSync(file, file + '.1'); } catch (e) {}
    fs.appendFileSync(file, line + '\n');
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

function mkdirp(dir) {
  if (fs.existsSync(dir)) return;
  mkdirp(path.dirname(dir));
  fs.mkdirSync(dir);
}

function writeFileAtomic(file, data) {
  mkdirp(path.dirname(file));
  var tmp = file + '.tmp';
  fs.writeFileSync(tmp, data);
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
  mkdirp(STATE_DIR);
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
    'mkdir -p ' + STATE_DIR,
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

var eventBuffer = [];
var eventOffset = 0;
var eventPartial = '';
var EVENT_BUFFER_MAX = 600;

function pumpEvents() {
  var file = STATE_DIR + '/events.jsonl';
  var st;
  try { st = fs.statSync(file); } catch (e) { return; }
  if (st.size < eventOffset) { eventOffset = 0; eventPartial = ''; }
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
    mkdirp(STATE_DIR);
    // Back (412) is always let through so the app can cancel the capture.
    var passthrough = Array.isArray(params.passthrough) ? params.passthrough : [412];
    writeFileAtomic(STATE_DIR + '/capture.json', JSON.stringify({ until: until, swallow: params.swallow !== false, passthrough: passthrough }));
    return { until: until };
  },

  stopCapture: function () {
    requireRoot();
    mkdirp(STATE_DIR);
    writeFileAtomic(STATE_DIR + '/capture.json', JSON.stringify({ until: 0 }));
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

  // Convenience for the TV app: elevate + autostart + daemon in one go.
  setup: function () {
    requireRoot();
    try { loadConfig(); } catch (e) { log('config: ' + e.message); }
    setAutostart(true);
    return ensureDaemon().then(function (d) {
      return { daemon: d, autostart: autostartStatus() };
    });
  },
};

function dispatch(method, params) {
  return new Promise(function (resolve) {
    if (!api[method]) throw new ApiError('unknown method ' + method, 'unknown_method');
    resolve(api[method](params || {}));
  });
}

Object.keys(api).forEach(function (method) {
  service.register(method, function (message) {
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

// Keep the service alive so status/events stay warm between app launches.
try {
  service.activityManager.create('keepAlive', function () {});
} catch (e) {
  log('activityManager unavailable: ' + e);
}

if (isRoot) {
  try {
    try { loadConfig(); } catch (e) { log('config: ' + e.message); }
    ensureDaemon().then(function (d) {
      log('daemon ' + (d.running ? 'running pid ' + d.pid : 'NOT running'));
    });
  } catch (e) {
    log('startup: ' + (e && e.stack || e));
  }
}

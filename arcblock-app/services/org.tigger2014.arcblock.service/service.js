/* ARC Sound Override Blocker service — runs as root on the TV (homebrew).
 * Single toggle: enabled = block auto-switching (incl. boot); disabled =
 * eim is never hooked and the service is inert (status only).
 * - keepAlive: stays running once started.
 * - start: ensure arcblock.so is injected into the eim holding /dev/cec0.
 * - autostart: idempotent boot-persistence via webosbrew init.d (same trick
 *   as lginputhook) — init.d wakes us with a luna call; a rapid 1s loop
 *   then injects the moment eim appears and stops (no steady polling).
 * - setEnabled: flips the toggle AND restarts eim live, so the change
 *   takes effect immediately: enabling re-injects into the fresh eim,
 *   disabling leaves a clean, never-hooked eim behind.
 * - getStatus: sound mode stays the status signal.
 * ES5 on purpose: the TV runs node 16.
 */
var pkgInfo = require('./package.json');
var Service = require('webos-service');
var child_process = require('child_process');
var fs = require('fs');
var path = require('path');

// Rendezvous with the hook: the app dir is world-writable, visible to both
// the jailed service and root. The jail can't touch /home/root etc., so all
// injection lives in this elevated service (+ init.d for boot).
var APPDIR = path.join(__dirname, '..', '..', 'applications', 'org.tigger2014.arcblock');
try {
  fs.accessSync(path.join(APPDIR, 'appinfo.json'));
} catch (e) {
  APPDIR = '/media/developer/apps/usr/palm/applications/org.tigger2014.arcblock';
}
var CFG = path.join(APPDIR, 'config.json');
var STATUSF = path.join(APPDIR, 'status.json');
var LASTF = path.join(APPDIR, 'last-output.json');
// Boot-restore grace: for this long after service start we enforce the saved
// output instead of tracking (boot flips TV->Beam before we can observe it).
var BOOT_GRACE_MS = 120000;
var bootTime = Date.now();

function isArcLike(v) {
  return typeof v === 'string' && /arc/i.test(v);
}
function isTvLike(v) {
  return typeof v === 'string' && /^tv_/i.test(v);
}
function readWanted() {
  try {
    var o = JSON.parse(fs.readFileSync(LASTF, 'utf8'));
    return (o && typeof o.wanted === 'string') ? o.wanted : null;
  } catch (e) { return null; }
}
function writeWanted(v) {
  try { fs.writeFileSync(LASTF, JSON.stringify({ wanted: v })); } catch (e) {}
}
function prevOutput() {
  try {
    var m = /"prevSoundOutput":"([^"]*)"/.exec(fs.readFileSync(PREFS, 'utf8'));
    return m ? m[1] : null;
  } catch (e) { return null; }
}
function trackOutput(cur) {
  // Don't learn during boot grace — that's the steal window, not the user.
  if (Date.now() - bootTime < BOOT_GRACE_MS) return;
  if (typeof cur !== 'string' || cur === '(unknown)') return;
  var wanted = readWanted();
  if (cur === wanted) return;
  // A vetoed steal (recent DROPs) must not retrain a TV wanted into ARC.
  if (wanted && isTvLike(wanted) && isArcLike(cur)) {
    var ev = lastEvents(5).join('\n');
    if (ev.indexOf('DROP') !== -1) return;
  }
  writeWanted(cur);
}
function maybeRestore() {
  if (!readEnabled()) return 'disabled';
  if (Date.now() - bootTime > BOOT_GRACE_MS) return 'grace-over';
  var wanted = readWanted();
  if (!wanted) {
    // No file yet: adopt prevSoundOutput (the pre-flip value) if it was TV.
    var prev = prevOutput();
    if (prev && isTvLike(prev)) { writeWanted(prev); wanted = prev; }
  }
  if (!wanted || !isTvLike(wanted)) return 'no-wanted';
  var cur = currentOutput();
  if (!isArcLike(cur)) return 'already-ok:' + cur;
  try {
    child_process.execFileSync('/usr/bin/luna-send', ['-n', '1',
      'luna://com.webos.settingsservice/setSystemSettings',
      JSON.stringify({ category: 'sound', settings: { soundOutput: wanted } })],
      { timeout: 15000 });
    return 'restored:' + cur + '->' + wanted;
  } catch (e) { return 'restore-failed:' + (e.message || e); }
}
var PREFS = '/var/luna/preferences/sound';
var LOG = '/tmp/arcblock.log';
var BINDIR = path.join(__dirname, 'bin');

function readEnabled() {
  // No config = OFF. The app must explicitly opt in; never self-enable.
  try {
    return fs.readFileSync(CFG, 'utf8').indexOf('"enabled":true') !== -1;
  } catch (e) { return false; }
}

function writeEnabled(v) {
  fs.writeFileSync(CFG, JSON.stringify({ enabled: !!v }));
}

function currentOutput() {
  try {
    var m = /"soundOutput":"([^"]*)"/.exec(fs.readFileSync(PREFS, 'utf8'));
    return m ? m[1] : '(unknown)';
  } catch (e) { return '(unknown)'; }
}

function eimPids() {
  try {
    var s = child_process.execSync('pidof eim', { encoding: 'utf8' }).trim();
    return s ? s.split(/\s+/) : [];
  } catch (e) { return []; }
}

function cecHolder() {
  var pids = eimPids();
  for (var i = 0; i < pids.length; i++) {
    try {
      var fds = fs.readdirSync('/proc/' + pids[i] + '/fd');
      for (var j = 0; j < fds.length; j++) {
        if (fs.readlinkSync('/proc/' + pids[i] + '/fd/' + fds[j]) === '/dev/cec0') return pids[i];
      }
    } catch (e) {}
  }
  return null;
}

function hookStatus() {
  // The hook publishes status.json; aliveness via kill(pid, 0) (EPERM =
  // alive under another uid, ESRCH = gone).
  var st = { hookedPid: 0, soundOutput: '', lastDrop: '' };
  try { st = JSON.parse(fs.readFileSync(STATUSF, 'utf8')); } catch (e) {}
  var alive = false;
  if (st.hookedPid) {
    try { process.kill(st.hookedPid, 0); alive = true; }
    catch (e) { alive = (e.code === 'EPERM'); }
  }
  return { pid: st.hookedPid || 0, alive: alive, lastDrop: st.lastDrop || '' };
}

function ensureInjected() {
  if (!readEnabled()) return 'disabled';
  var holder = cecHolder();
  if (!holder) return 'no-holder';
  var hs = hookStatus();
  if (hs.alive && String(hs.pid) === String(holder)) return 'already:' + holder;
  try {
    var maps = fs.readFileSync('/proc/' + holder + '/maps', 'utf8');
    if (maps.indexOf('arcblock.so') !== -1) return 'already-maps:' + holder;
  } catch (e) {}
  try {
    child_process.execFileSync(path.join(BINDIR, 'ezinject'), [holder, path.join(BINDIR, 'arcblock.so')], { timeout: 90000 });
    return 'injected:' + holder;
  } catch (e) { return 'inject-failed:' + (e.message || e); }
}

function lastEvents(n) {
  try {
    var lines = fs.readFileSync(LOG, 'utf8').split('\n');
    var out = [];
    for (var i = lines.length - 1; i >= 0 && out.length < n; i--) {
      if (lines[i].indexOf('DROP') !== -1 || lines[i].indexOf('config enabled') !== -1) out.push(lines[i]);
    }
    return out.reverse();
  } catch (e) { return []; }
}

var service = new Service(pkgInfo.name);
service.activityManager.create('keepAlive', function (activity) {});

service.register('start', function (message) {
  var r, rs;
  try { r = ensureInjected(); } catch (e) { r = 'error:' + e.message; }
  try { rs = maybeRestore(); } catch (e) { rs = 'error:' + e.message; }
  try { armTraps(); } catch (e) {}
  message.respond({ returnValue: true, ensure: r, restore: rs });
});

service.register('autostart', function (message) {
  try {
    var dir = '/var/lib/webosbrew/init.d';
    try { fs.mkdirSync(dir, { recursive: true }); } catch (e) {}
    var script = '#!/bin/sh\n' +
      '# ARC Sound Override Blocker boot persistence (written by the service itself).\n' +
      'for i in 1 2 3 4 5 6; do\n' +
      '  if luna-send -n 1 \'luna://org.tigger2014.arcblock.service/start\' \'{}\' 2>/dev/null | grep -q \'"returnValue":true\'; then exit 0; fi\n' +
      '  sleep 10\ndone\n';
    fs.writeFileSync(dir + '/arcblock', script, { mode: 493 });
    try { fs.chmodSync(dir + '/arcblock', '755'); } catch (e) {}
    message.respond({ returnValue: true });
  } catch (e) {
    message.respond({ returnValue: false, errorText: String((e && e.stack) || e) });
  }
});

function waitHolderAsync(ms, cb) {
  var waited = 0;
  (function poll() {
    var h = null;
    try { h = cecHolder(); } catch (e) {}
    if (h || waited >= ms) { cb(h); return; }
    waited += 1000;
    setTimeout(poll, 1000);
  })();
}

service.register('setEnabled', function (message) {
  var p = message.payload || {};
  try { writeEnabled(!!p.enabled); } catch (e) {
    message.respond({ returnValue: false, errorText: String(e.message || e) });
    return;
  }
  var en = readEnabled();
  // Hooks can't unload from a live process: bounce eim so the toggle bites.
  var rr;
  try {
    child_process.execFileSync('/bin/systemctl', ['restart', 'eim'], { timeout: 60000 });
    rr = 'restarted';
  } catch (e) {
    message.respond({ returnValue: true, enabled: en, restart: 'restart-failed:' + (e.message || e) });
    return;
  }
  waitHolderAsync(30000, function (holder) {
    if (!holder) {
      message.respond({ returnValue: true, enabled: en, restart: rr + ':no-holder' });
      return;
    }
    // Fresh eim, old hooks died with it: inject only when enabled.
    var r = 'skipped:disabled', rs = 'skipped:disabled';
    if (en) {
      try { r = ensureInjected(); } catch (e) { r = 'error:' + e.message; }
      try { rs = maybeRestore(); } catch (e) { rs = 'error:' + e.message; }
      try { armTraps(); } catch (e) {}
    }
    message.respond({ returnValue: true, enabled: en, restart: rr + ':' + holder, ensure: r, restore: rs });
  });
});

service.register('getStatus', function (message) {
  var hs = hookStatus();
  var cur = currentOutput();
  if (readEnabled()) { try { trackOutput(cur); } catch (e) {} }
  message.respond({
    returnValue: true,
    enabled: readEnabled(),
    soundOutput: cur,
    wanted: readWanted(),
    holder: cecHolder(),
    hooked: hs.alive ? [String(hs.pid)] : [],
    hookPid: hs.pid,
    shutdown: { app: !!(shutdownReg.app && shutdownReg.app.id), svc: !!(shutdownReg.svc && shutdownReg.svc.id) },
    recent: lastEvents(5)
  });
});

service.register('debugShutdown', function (message) {
  var out = {};
  var buses = ['luna://com.palm.sleep/shutdown/', 'luna://com.webos.service.sleep/shutdown/', 'luna://com.webos.service.power/shutdown/'];
  var pending = buses.length;
  function done() {
    pending--;
    if (pending <= 0) {
      message.respond({ returnValue: true, probes: out, log: shutdownLog,
        reg: { app: !!(shutdownReg.app && shutdownReg.app.id), svc: !!(shutdownReg.svc && shutdownReg.svc.id) } });
    }
  }
  buses.forEach(function (base) {
    try {
      var seen = [];
      service.call(base + 'shutdownApplicationsRegister',
        { clientName: 'org.tigger2014.arcblock-probe', subscribe: true },
        function (m) {
          var p = {}; try { p = (m && m.payload) || {}; } catch (e) {}
          seen.push(p);
          if (seen.length >= 1) { out[base] = seen; done(); }
        });
      setTimeout(function () {
        if (!out[base]) { out[base] = seen.length ? seen : [{ timeout: true }]; done(); }
      }, 8000);
    } catch (e) { out[base] = [{ threw: String(e && e.message || e) }]; done(); }
  });
});

service.register('debug', function (message) {
  var r = { uid: -1, gid: -1, user: null, pidof: null, initd: null, fd: null, proc: 0, eimFd: null };
  try { r.uid = process.getuid(); r.gid = process.getgid(); } catch (e) {}
  r.user = process.env.USER || null;
  try { r.pidof = child_process.execSync('command -v pidof || which pidof', { encoding: 'utf8' }).trim(); }
  catch (e) { r.pidof = 'ERR:' + e.message; }
  try { fs.accessSync('/var/lib/webosbrew/init.d', fs.constants.W_OK); r.initd = 'writable'; }
  catch (e) { r.initd = 'ERR:' + e.code; }
  try { r.fd = 'readable:' + fs.readdirSync('/proc/1/fd').length; }
  catch (e) { r.fd = 'ERR:' + e.code; }
  try {
    var holders = [];
    var pids = child_process.execSync('pidof eim', { encoding: 'utf8' }).trim().split(/\s+/);
    r.eimFd = 'pids=' + pids.join(',');
  } catch (e) { r.eimFd = 'ERR:' + String(e.message).slice(0, 80); }
  try { r.proc = fs.readdirSync('/proc').filter(function (x) { return /^[0-9]+$/.test(x); }).length; } catch (e) {}
  message.respond({ returnValue: true, debug: r });
});

// Boot: inject the moment the eim holding /dev/cec0 appears, then stop.
// No steady-state polling.
var bootTries = 0;
var bootTimer = null;
// Disabled at load = service stays inert: no inject, no restore, no traps.
// The toggle path (setEnabled) restarts eim and arms everything itself.
if (readEnabled()) {
  bootTimer = setInterval(function () {
    bootTries++;
    var r = 'none';
    try { r = ensureInjected(); } catch (e) { r = 'error'; }
    try { maybeRestore(); } catch (e) {}
    // Stop once eim was here (injected or already hooked) or after ~150s.
    if (r !== 'no-holder' || bootTries > 150) clearInterval(bootTimer);
  }, 1000);
}

// Shutdown trap (save output before power-off), two layers:
// 1. sleepd handshake (per OSE source + LS2 docs): register for the
//    shutdownApplications signal, save on receipt, ack with our clientId.
//    15s timeouts mean a lost ack can't hold up shutdown.
// 2. Backstop: tvpower getPowerState subscription; any leave-Active saves.
var shutdownReg = { app: null, svc: null };
var shutdownDone = {};
var shutdownLog = [];
function slog(s) { shutdownLog.push(s); if (shutdownLog.length > 12) shutdownLog.shift(); }
function saveStateNow(reason) {
  if (!readEnabled()) return 'disabled';
  try {
    var cur = currentOutput();
    if (typeof cur === 'string' && cur !== '(unknown)') {
      writeWanted(cur);
      return 'saved:' + cur + ':' + reason;
    }
  } catch (e) {}
  return 'save-failed:' + reason;
}
function shutdownRegister(bus, kind, ackMethod) {
  // Registers on every known bus name for sleepd; unknown ones just error.
  var uris = [
    'luna://com.palm.sleep/shutdown/',
    'luna://com.webos.service.sleep/shutdown/',
    'luna://com.webos.service.power/shutdown/'
  ];
  uris.forEach(function (base) {
    if (shutdownDone[kind + '|' + base]) return;
    slog('call:' + kind + '|' + base);
    try {
      service.call(base + kind,
        { clientName: 'org.tigger2014.arcblock', subscribe: true },
        function (m) {
          var p = {};
          try { p = (m && m.payload) || {}; } catch (e) { slog('parse-err:' + base); }
          slog('cb:' + base + ':' + JSON.stringify(p).slice(0, 80));
          if (p.clientId) {
            // Registration reply: remember our id for the ack.
            shutdownDone[kind + '|' + base] = true;
            if (kind.charAt(8) === 'A') shutdownReg.app = { uri: base, id: p.clientId, ack: ackMethod };
            else shutdownReg.svc = { uri: base, id: p.clientId, ack: ackMethod };
          } else if (p.returnValue !== false) {
            // The shutdown signal itself (payload "{}"): save first, ack.
            saveStateNow('shutdown:' + kind);
            var reg = (kind.charAt(8) === 'A') ? shutdownReg.app : shutdownReg.svc;
            if (reg && reg.id && reg.uri === base) {
              try {
                service.call(base + ackMethod, { clientId: reg.id }, function () {});
              } catch (e) {}
            }
          }
          // else: error payload (unknown bus name etc.) — ignore.
        });
    } catch (e) { slog('threw:' + base + ':' + String(e && e.message || e).slice(0, 60)); }
  });
}
function doShutdownRegister() {
  // Apps tier only: shutdownServicesRegister rejects >1-key payloads, so our
  // library's subscribe:true gets "Invalid parameters" (probed on-device).
  // The apps signal fires first anyway — better save timing.
  try { shutdownRegister(null, 'shutdownApplicationsRegister', 'shutdownApplicationsAck'); } catch (e) {}
}
// Trap arming. Delayed: top-of-load service.call can race the bus connection
// (registration silently never lands). Retried from 'start'; per-bus once a
// clientId is banked; never while disabled.
var trapsArmed = false;
function armTraps() {
  if (trapsArmed || !readEnabled()) return;
  trapsArmed = true;
  try { doShutdownRegister(); } catch (e) {}
  try {
    service.call('luna://com.webos.service.tvpower/power/getPowerState',
      { subscribe: true },
      function (m) {
        try {
          var p = (m && m.payload) || {};
          if (p.state && p.state !== 'Active') saveStateNow('power:' + p.state);
        } catch (e) {}
      });
  } catch (e) {}
}
setTimeout(function () { try { armTraps(); } catch (e) {} }, 5000);

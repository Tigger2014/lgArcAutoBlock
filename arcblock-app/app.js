/* ARC Sound Override Blocker frontend — talks to our Luna service, nothing else. */
(function () {
  'use strict';
  var SVC = 'luna://org.tigger2014.arcblock.service/';
  var enabled = true;

  function $(id) { return document.getElementById(id); }

  function call(method, params) {
    return new Promise(function (resolve) {
      var bridge = new WebOSServiceBridge();
      bridge.onservicecallback = function (msg) {
        var r;
        try { r = JSON.parse(msg); }
        catch (e) { r = { returnValue: false, errorText: 'bad json' }; }
        resolve(r);
      };
      bridge.call(SVC + method, JSON.stringify(params || {}));
    });
  }

  function render(s) {
    enabled = !!s.enabled;
    var t = $('toggle');
    t.disabled = false;
    t.textContent = enabled ? 'Blocker ON' : 'Blocker OFF';
    t.className = 'toggle ' + (enabled ? 'on' : 'off');
    $('stEnabled').textContent = enabled ? 'loaded' : 'off';
    var h = s.hooked || [];
    $('stHooked').textContent = h.length ? 'yes (pid ' + h.join(',') + ')' : 'no';
    var recent = s.recent || [];
    $('recent').textContent = recent.length ? recent.join('\n') : '(quiet)';
  }

  function fail(e) {
    $('err').textContent = 'service error: ' + (e && (e.errorText || e.message) || e);
  }

  function refresh() {
    call('getStatus', {}).then(function (s) {
      if (!s.returnValue) { fail(s); return; }
      $('err').textContent = '';
      render(s);
    }).catch(fail);
  }

  $('toggle').addEventListener('click', function () {
    var t = $('toggle');
    t.disabled = true;
    t.textContent = 'Working… (eim restarts, ~30s)';
    call('setEnabled', { enabled: !enabled }).then(function (s) {
      if (!s.returnValue) { fail(s); }
      refresh();
      try { t.focus(); } catch (e) {}
    }).catch(fail);
  });
  document.addEventListener('keydown', function (e) {
    if (e.keyCode === 13 || e.keyCode === 415) {
      var t = $('toggle');
      if (document.activeElement !== t) { try { t.focus(); } catch (err) {} }
    }
  });

  function firstLaunch() {
    // Mirror lginputhook: elevate the service to root via Homebrew Channel,
    // then register boot persistence + ensure injection.
    var hb = 'luna://org.webosbrew.hbchannel.service/';
    var b = new WebOSServiceBridge();
    b.onservicecallback = function (msg) {
      var cfg;
      try { cfg = JSON.parse(msg); } catch (e) { cfg = {}; }
      if (!cfg.root) { fail('homebrew channel is not rooted'); refresh(); return; }
      var e = new WebOSServiceBridge();
      e.onservicecallback = function (m2) {
        call('autostart', {}).catch(function () {});
        call('start', {}).then(refresh).catch(fail);
      };
      e.call(hb + 'exec', JSON.stringify({
        command: '/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service/elevate-service org.tigger2014.arcblock.service'
      }));
    };
    b.call(hb + 'getConfiguration', '{}');
  }

  // First launch: elevate + boot persistence + ensure injection, then state.
  firstLaunch();
  setInterval(refresh, 5000);
})();

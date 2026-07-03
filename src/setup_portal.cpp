#include "setup_portal.h"
#include "config_api.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

// WPA2 requires 8+ characters. Change this if you want a different one -
// it's only ever needed briefly during setup.
static const char *AP_SSID = "LR2-Redux-Setup";
static const char *AP_PASSWORD = "lr2setup";
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);

static DNSServer dnsServer;
static AsyncWebServer portalServer(80);

// Minimal, self-contained (no external requests) - the AP has no internet
// access, so nothing here can load from a CDN. Dark-only to keep it small;
// this is a one-time setup screen, not something worth theming.
static const char SETUP_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LR2-Redux Setup</title>
<style>
  body { font-family: -apple-system, "Segoe UI", Roboto, sans-serif; max-width: 420px; margin: 24px auto; padding: 0 16px 40px; background: #0d241a; color: #eaf3ec; }
  h1 { font-size: 19px; margin-bottom: 4px; }
  p.lede { color: #93ab9c; font-size: 13px; margin-top: 0; }
  label { display: block; margin-top: 14px; font-size: 12px; color: #93ab9c; text-transform: uppercase; letter-spacing: 0.04em; }
  input { width: 100%; box-sizing: border-box; padding: 9px; margin-top: 4px; border-radius: 4px; border: 1px solid #24503c; background: #163d2e; color: #eaf3ec; font-size: 14px; }
  button { margin-top: 10px; width: 100%; padding: 10px; border: none; border-radius: 4px; background: #d99a5c; color: #1c2e26; font-weight: 600; font-size: 14px; }
  button.secondary { background: transparent; border: 1px solid #d99a5c; color: #d99a5c; }
  .net { padding: 8px; border: 1px solid #24503c; border-radius: 4px; margin-top: 6px; cursor: pointer; font-size: 13px; display: flex; justify-content: space-between; }
  .net:active { background: #163d2e; }
  .status { margin-top: 14px; font-size: 13px; color: #93ab9c; }
  .row { display: flex; gap: 10px; }
  .row > div { flex: 1; }
</style>
</head><body>
  <h1>LR2-Redux Setup</h1>
  <p class="lede">Configure WiFi and MQTT, then save to reboot onto your network.</p>

  <label>WiFi network</label>
  <input id="ssid" placeholder="SSID">
  <button type="button" class="secondary" onclick="scanNetworks()">Scan for networks</button>
  <div id="scanList"></div>

  <label>WiFi password</label>
  <input id="wifiPass" type="password" placeholder="leave blank to keep current">

  <label>MQTT host</label>
  <input id="mqttHost" placeholder="192.168.1.10">

  <div class="row">
    <div>
      <label>MQTT port</label>
      <input id="mqttPort" type="number" value="1883">
    </div>
    <div>
      <label>Wait time (minutes)</label>
      <input id="waitMin" type="number" min="1" max="60" value="7">
    </div>
  </div>

  <label>MQTT username (optional)</label>
  <input id="mqttUser">

  <label>MQTT password (optional)</label>
  <input id="mqttPass" type="password" placeholder="leave blank to keep current">

  <button onclick="save()">Save &amp; reboot</button>
  <p id="msg" class="status"></p>

<script>
fetch('/config').then(function(r) { return r.json(); }).then(function(c) {
  document.getElementById('ssid').value = c.wifiSsid || '';
  document.getElementById('mqttHost').value = c.mqttHost || '';
  document.getElementById('mqttPort').value = c.mqttPort || 1883;
  document.getElementById('mqttUser').value = c.mqttUser || '';
  document.getElementById('waitMin').value = c.waitTimerMin || 7;
});

function scanNetworks() {
  document.getElementById('scanList').innerHTML = '<div class="status">Scanning…</div>';
  pollScan();
}
function pollScan() {
  fetch('/scan').then(function(r) { return r.json(); }).then(function(d) {
    if (d.status === 'scanning') { setTimeout(pollScan, 1500); return; }
    var list = document.getElementById('scanList');
    list.innerHTML = '';
    (d.networks || []).sort(function(a, b) { return b.rssi - a.rssi; }).forEach(function(n) {
      var el = document.createElement('div');
      el.className = 'net';
      el.innerHTML = '<span>' + n.ssid + '</span><span>' + n.rssi + ' dBm' + (n.secure ? ' 🔒' : '') + '</span>';
      el.onclick = function() { document.getElementById('ssid').value = n.ssid; };
      list.appendChild(el);
    });
  });
}

function save() {
  var body = {
    wifiSsid: document.getElementById('ssid').value,
    wifiPass: document.getElementById('wifiPass').value,
    mqttHost: document.getElementById('mqttHost').value,
    mqttPort: parseInt(document.getElementById('mqttPort').value, 10) || 1883,
    mqttUser: document.getElementById('mqttUser').value,
    mqttPass: document.getElementById('mqttPass').value,
    waitTimerMin: parseInt(document.getElementById('waitMin').value, 10) || 7
  };
  document.getElementById('msg').textContent = 'Saving…';
  fetch('/save', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) })
    .then(function() { document.getElementById('msg').textContent = 'Saved. Rebooting onto your WiFi — this page will stop working once the AP drops.'; });
}
</script>
</body></html>
)HTML";

static void handleRoot(AsyncWebServerRequest *request) {
  request->send_P(200, "text/html", SETUP_HTML);
}

void startSetupPortal(DeviceConfig &cfg) {
  WiFi.mode(WIFI_AP_STA); // AP_STA (not plain AP) so /scan can see nearby networks while the portal is up
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  dnsServer.start(53, "*", AP_IP);

  portalServer.on("/", HTTP_GET, handleRoot);
  registerConfigApiRoutes(portalServer, cfg);
  portalServer.onNotFound(handleRoot); // catch-all so captive-portal detection pops the page

  portalServer.begin();
}

void setupPortalLoop() {
  dnsServer.processNextRequest();
}

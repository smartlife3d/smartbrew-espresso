// ==========================================================================
//  Smart Brew Cockpit — Amazon Echo Show 5 Native JavaScript Bridge
// ==========================================================================
(function() {
  'use strict';

  // UI-19: Prevent duplicate initialization of bridge and listeners
  if (window.__echo5BridgeInstalled) {
    console.log("[Echo5Bridge] Already installed, skipping duplicate initialization.");
    return;
  }
  window.__echo5BridgeInstalled = true;

  console.log("[Echo5Bridge] Initializing Echo Show 5 Native Bridge...");

  const isNative = typeof window.AndroidScale !== 'undefined' || typeof window.EchoCompanion !== 'undefined';
  const bridge = window.AndroidScale || window.EchoCompanion;

  if (!isNative) {
    console.log("[Echo5Bridge] Running in standard browser mode (no native bridge).");
    return;
  }

  let mcuHost = (bridge.getMcuHost && bridge.getMcuHost()) ? bridge.getMcuHost().trim() : "smartbrew.local";
  if (!mcuHost || mcuHost === "") {
    mcuHost = "smartbrew.local";
  }

  // --------------------------------------------------------------------------
  // 1. Transparent Network Routing for Local Asset Mode
  // --------------------------------------------------------------------------
  const isLocalOrigin = location.protocol === "file:" || location.hostname === "appassets.androidplatform.net" || location.host === "";

  if (isLocalOrigin) {
    console.log("[Echo5Bridge] Routing network calls to Controller: " + mcuHost);

    // Monkey-patch window.fetch to route API calls directly to Controller
    const originalFetch = window.fetch;
    window.fetch = function(resource, init) {
      if (typeof resource === 'string') {
        if (resource.startsWith('/')) {
          resource = "http://" + mcuHost + resource;
        } else if (resource.startsWith('http://192.168.4.1/') || resource.startsWith('http://smartbrew.local/') || resource.startsWith('http://brew-mcu.local/') || resource.startsWith('http://elizabeth.local/')) {
          resource = resource.replace(/^http:\/\/[^/]+/, "http://" + mcuHost);
        }
      } else if (resource && typeof resource.url === 'string') {
        let u = resource.url;
        if (u.startsWith('http://appassets.androidplatform.net/') || u.startsWith('https://appassets.androidplatform.net/')) {
          u = u.replace(/^https?:\/\/appassets\.androidplatform\.net/, "http://" + mcuHost);
          resource = new Request(u, resource);
        } else if (u.startsWith('http://192.168.4.1/') || u.startsWith('http://smartbrew.local/') || u.startsWith('http://brew-mcu.local/') || u.startsWith('http://elizabeth.local/')) {
          u = u.replace(/^http:\/\/[^/]+/, "http://" + mcuHost);
          resource = new Request(u, resource);
        }
      }
      return originalFetch.call(this, resource, init);
    };

    // Monkey-patch WebSocket to connect to Controller (UI-20: Preserve static constants)
    const OriginalWebSocket = window.WebSocket;
    function PatchedWebSocket(url, protocols) {
      if (typeof url === 'string' && url.includes('/ws')) {
        url = "ws://" + mcuHost + "/ws";
      }
      console.log("[Echo5Bridge] Opening WebSocket to: " + url);
      const socket = new OriginalWebSocket(url, protocols);
      window.ws = socket;
      socket.addEventListener('open', () => {
        console.log("[Echo5Bridge] WebSocket OPENED successfully to " + url);
        window.wsConnected = true;
        updateMcuCompanionStatus(true);
      });
      socket.addEventListener('close', (e) => {
        console.log("[Echo5Bridge] WebSocket CLOSED code=" + e.code + " reason=" + e.reason);
        window.wsConnected = false;
        updateMcuCompanionStatus(false);
      });
      socket.addEventListener('error', (e) => {
        console.log("[Echo5Bridge] WebSocket ERROR on " + url);
        window.wsConnected = false;
        updateMcuCompanionStatus(false);
      });
      return socket;
    }
    PatchedWebSocket.prototype = OriginalWebSocket.prototype;
    // UI-20: Preserve static constants (CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3)
    ['CONNECTING', 'OPEN', 'CLOSING', 'CLOSED'].forEach(function(k) {
      PatchedWebSocket[k] = OriginalWebSocket[k];
    });
    window.WebSocket = PatchedWebSocket;
  }

  // --------------------------------------------------------------------------
  // 2. Dynamic Controller Target Switcher (Triggered by Native Auto-Discovery)
  // --------------------------------------------------------------------------
  window.setMcuTarget = function(newHost) {
    if (!newHost || typeof newHost !== 'string') return;
    newHost = newHost.trim();
    if (!newHost || newHost === mcuHost) return;

    console.log("[Echo5Bridge] Switching Controller target to: " + newHost);
    mcuHost = newHost;

    // Immediately reconnect WebSocket to the newly discovered host
    window.__forceWsReconnect = true;
    if (typeof window.connectWebSocket === 'function') {
      if (window.ws) {
        try { window.ws.close(); } catch(e) {}
        window.ws = null;
      }
      window.connectWebSocket();
    }

    // Refresh telemetry and profile state
    if (typeof window.loadProfiles === 'function') {
      try { window.loadProfiles(); } catch(e) {}
    } else if (typeof window.fetchProfiles === 'function') {
      try { window.fetchProfiles(); } catch(e) {}
    }
    if (typeof window.loadLedger === 'function') {
      try { window.loadLedger(); } catch(e) {}
    } else if (typeof window.fetchHistory === 'function') {
      try { window.fetchHistory(); } catch(e) {}
    }
    if (typeof window.fetchSystemData === 'function') {
      try { window.fetchSystemData(); } catch(e) {}
    } else if (typeof window.fetchStatus === 'function') {
      try { window.fetchStatus(); } catch(e) {}
    }
  };

  window.onMcuDiscovered = function(data) {
    if (data && data.host) {
      window.setMcuTarget(data.host);
    }
  };

  // --------------------------------------------------------------------------
  // 3. Scale & Telemetry Note:
  // The BLE scale is connected directly to the ESP32 controller (not the client device).
  // Live weight is streamed over WebSocket (m.weight_g), and tare commands are
  // sent to the Controller via { cmd: "TARE" }.
  // --------------------------------------------------------------------------

  // --------------------------------------------------------------------------
  // 6. UI Toolbar Button Injection & Settings Companion Panel
  // --------------------------------------------------------------------------
  function updateMcuCompanionStatus(online) {
    const statusEl = document.getElementById("mcu-companion-status");
    if (!statusEl) return;

    let isConnected = false;
    if (typeof online === "boolean") {
      isConnected = online;
    } else {
      const pill = document.getElementById("mcu-status-pill");
      const dot = document.getElementById("mcu-status-dot");
      const text = document.getElementById("mcu-status-text");
      isConnected = (window.wsConnected === true) ||
                    (window.ws && window.ws.readyState === 1) ||
                    (pill && pill.classList.contains("connected")) ||
                    (dot && dot.classList.contains("connected")) ||
                    (text && text.textContent.trim().toLowerCase() === "connected");
    }

    if (isConnected) {
      statusEl.innerHTML = '<span style="color:#10b981;font-weight:700;">● Connected (Online)</span>';
    } else {
      statusEl.innerHTML = '<span style="color:#f59e0b;font-weight:700;">◌ Connecting…</span>';
    }
  }
  window.updateMcuCompanionStatus = updateMcuCompanionStatus;

  window.onMcuStatus = function(data) {
    if (typeof data === 'string') {
      try { data = JSON.parse(data); } catch(e) { data = {}; }
    }
    const isOnline = !!(data && data.online);
    updateMcuCompanionStatus(isOnline);
  };

  function setupMcuCompanionSettings() {
    const wifiCur = document.getElementById("wifi-current-group");
    if (wifiCur) wifiCur.style.display = "none";
    const wifiJoin = document.getElementById("wifi-join-group");
    if (wifiJoin) wifiJoin.style.display = "none";

    // Hostname/mDNS configuration is for controller web only; hide on companion
    const hostRow = document.getElementById("setting-row-mcu-hostname");
    if (hostRow) hostRow.style.display = "none";

    const mcuGroup = document.getElementById("mcu-companion-group");
    if (mcuGroup) mcuGroup.style.display = "block";

    const targetEl = document.getElementById("mcu-companion-target");
    if (targetEl) targetEl.textContent = mcuHost;

    updateMcuCompanionStatus();

    const rescanBtn = document.getElementById("btn-mcu-rescan");
    if (rescanBtn) {
      rescanBtn.onclick = function() {
        rescanBtn.textContent = "↻ Scanning…";
        if (bridge && bridge.discoverMcu) bridge.discoverMcu();
        setTimeout(() => { rescanBtn.textContent = "↻ Scan for Controllers"; }, 3500);
      };
    }

    const selectBtn = document.getElementById("btn-mcu-select-connect");
    const selectList = document.getElementById("mcu-select-list");
    if (selectBtn && selectList) {
      selectBtn.onclick = function() {
        const val = selectList.value;
        if (val) {
          if (bridge && bridge.setMcuHost) bridge.setMcuHost(val);
          window.setMcuTarget(val);
          if (targetEl) targetEl.textContent = val;
          if (typeof window.showToast === 'function') window.showToast("Switched Controller to " + val);
        }
      };
    }

    const manualBtn = document.getElementById("btn-mcu-save-manual");
    const manualInput = document.getElementById("mcu-manual-input");
    if (manualBtn && manualInput) {
      manualBtn.onclick = function() {
        const host = manualInput.value.trim();
        if (host) {
          if (bridge && bridge.setMcuHost) bridge.setMcuHost(host);
          window.setMcuTarget(host);
          if (targetEl) targetEl.textContent = host;
          if (typeof window.showToast === 'function') window.showToast("Connecting to Controller: " + host);
        }
      };
    }

    // Populate with any already known Controllers
    if (bridge && bridge.getDiscoveredMcus) {
      try {
        const raw = bridge.getDiscoveredMcus();
        if (raw) window.onMcuListDiscovered(raw);
      } catch (e) {}
    }
  }

  window.onMcuListDiscovered = function(mcus) {
    if (typeof mcus === 'string') {
      try { mcus = JSON.parse(mcus); } catch (e) { mcus = []; }
    }
    if (!Array.isArray(mcus)) return;
    const sel = document.getElementById("mcu-select-list");
    if (!sel) return;
    sel.innerHTML = "";
    if (mcus.length === 0) {
      sel.innerHTML = '<option value="">None found</option>';
      return;
    }
    mcus.forEach(m => {
      const opt = document.createElement("option");
      opt.value = m.ip;
      const isCurrent = (m.ip === mcuHost || m.hostname === mcuHost);
      const hostName = m.hostname || "smartbrew";
      opt.textContent = `${isCurrent ? "✓ " : ""}${hostName} (${m.ip})`;
      if (isCurrent) opt.selected = true;
      sel.appendChild(opt);
    });
  };

  function setupToolbarButtons() {
    // Hook Settings button to initialize the Controller companion panel
    const settingsBtn = document.getElementById("btn-settings");
    if (settingsBtn) {
      settingsBtn.addEventListener("click", () => {
        setTimeout(setupMcuCompanionSettings, 50);
      });
    }

    setupMcuCompanionSettings();
    setInterval(() => updateMcuCompanionStatus(), 1500);
  }

  // --------------------------------------------------------------------------
  // 5. Initialization
  // --------------------------------------------------------------------------
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", setupToolbarButtons);
  } else {
    setupToolbarButtons();
  }

})();

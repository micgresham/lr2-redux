import { useCallback, useState } from "react";

export interface DeviceConfigPayload {
  wifiSsid: string;
  mqttHost: string;
  mqttPort: number;
  mqttUser: string;
  waitTimerMin: number;
  requireManualReset: boolean;
  catPresentWarningMin: number;
  dayStartHour: number;
  dayEndHour: number;
  drawerFullCycles: number;
  homeOvershootMs: number;
  dumpShakeStepMs: number;
  dumpShakeCount: number;
}

export interface WifiNetwork {
  ssid: string;
  rssi: number;
  secure: boolean;
}

// Optional throughout: the device's /save handler only overwrites fields
// present in the request body and leaves everything else as-is, so callers
// that only care about one setting (e.g. the Analytics page's day/night
// hours) can post just that without needing the full WiFi/MQTT shape.
export interface SaveConfigInput {
  wifiSsid?: string;
  wifiPass?: string;
  mqttHost?: string;
  mqttPort?: number;
  mqttUser?: string;
  mqttPass?: string;
  waitTimerMin?: number;
  requireManualReset?: boolean;
  catPresentWarningMin?: number;
  dayStartHour?: number;
  dayEndHour?: number;
  drawerFullCycles?: number;
  homeOvershootMs?: number;
  dumpShakeStepMs?: number;
  dumpShakeCount?: number;
}

// The config HTTP API lives on the same host as the WS endpoint (ws(s)://host/ws).
function toHttpBase(deviceUrl: string): string {
  return deviceUrl.replace(/^ws/, "http").replace(/\/ws\/?$/, "");
}

// The device calls ESP.restart() immediately after responding to /save -
// that reboot frequently tears down the TCP connection before the browser
// finishes reading the response, which fetch() surfaces as a network error
// even though the save itself went through fine. So a failed /save request
// isn't treated as a real error - instead, save() polls /config afterward
// until the device comes back (typical reboot + WiFi reconnect time), so
// the page picks up the fresh values on its own instead of needing a manual
// refresh. Give up and report a real error only if it never comes back.
const RECONNECT_POLL_MS = 3000;
const RECONNECT_MAX_ATTEMPTS = 10; // ~30s of polling before giving up

export function useDeviceConfigApi(deviceUrl: string) {
  const base = toHttpBase(deviceUrl);
  const [config, setConfig] = useState<DeviceConfigPayload | null>(null);
  const [networks, setNetworks] = useState<WifiNetwork[]>([]);
  const [scanning, setScanning] = useState(false);
  const [saving, setSaving] = useState(false);
  const [reconnecting, setReconnecting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const fetchConfig = useCallback(async () => {
    try {
      const res = await fetch(`${base}/config`);
      const data = (await res.json()) as DeviceConfigPayload;
      setConfig(data);
      setError(null);
    } catch {
      setError("Couldn't load config from the device");
    }
  }, [base]);

  const scan = useCallback(() => {
    setScanning(true);
    setError(null);

    const poll = () => {
      fetch(`${base}/scan`)
        .then((res) => res.json())
        .then((data: { status?: string; networks?: WifiNetwork[] }) => {
          if (data.status === "scanning") {
            setTimeout(poll, 1500);
            return;
          }
          setNetworks((data.networks ?? []).slice().sort((a, b) => b.rssi - a.rssi));
          setScanning(false);
        })
        .catch(() => {
          setError("Scan failed");
          setScanning(false);
        });
    };
    poll();
  }, [base]);

  // skipReconnectPoll: pass true when the save changes the WiFi SSID - the
  // device reboots onto a *different* network in that case, so polling this
  // same base URL would never succeed and would just produce a spurious
  // timeout error instead of the "reconnect on the new network" message the
  // caller already shows.
  const save = useCallback(
    async (input: SaveConfigInput, opts?: { skipReconnectPoll?: boolean }) => {
      setSaving(true);
      setError(null);
      try {
        await fetch(`${base}/save`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(input),
        });
      } catch {
        // Expected most of the time - see the comment above. Not a real
        // error on its own; only actually a problem if the device never
        // comes back (checked below).
      } finally {
        setSaving(false);
      }

      if (opts?.skipReconnectPoll) return true;

      setReconnecting(true);
      for (let attempt = 0; attempt < RECONNECT_MAX_ATTEMPTS; attempt++) {
        await new Promise((r) => setTimeout(r, RECONNECT_POLL_MS));
        try {
          const res = await fetch(`${base}/config`);
          const data = (await res.json()) as DeviceConfigPayload;
          setConfig(data);
          setReconnecting(false);
          return true;
        } catch {
          // still rebooting/reconnecting - keep trying
        }
      }
      setReconnecting(false);
      setError("Device didn't come back after saving - check it's powered and reconnected to WiFi.");
      return false;
    },
    [base],
  );

  return {
    config,
    networks,
    scanning,
    saving,
    reconnecting,
    error,
    fetchConfig,
    scan,
    save,
  };
}

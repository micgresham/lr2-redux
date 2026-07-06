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
}

// The config HTTP API lives on the same host as the WS endpoint (ws(s)://host/ws).
function toHttpBase(deviceUrl: string): string {
  return deviceUrl.replace(/^ws/, "http").replace(/\/ws\/?$/, "");
}

export function useDeviceConfigApi(deviceUrl: string) {
  const base = toHttpBase(deviceUrl);
  const [config, setConfig] = useState<DeviceConfigPayload | null>(null);
  const [networks, setNetworks] = useState<WifiNetwork[]>([]);
  const [scanning, setScanning] = useState(false);
  const [saving, setSaving] = useState(false);
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

  const save = useCallback(
    async (input: SaveConfigInput) => {
      setSaving(true);
      setError(null);
      try {
        await fetch(`${base}/save`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(input),
        });
        return true;
      } catch {
        setError("Save failed - is the device reachable?");
        return false;
      } finally {
        setSaving(false);
      }
    },
    [base],
  );

  return { config, networks, scanning, saving, error, fetchConfig, scan, save };
}

import { useCallback, useEffect, useState } from "react";

export interface AnalyticsPayload {
  hourly24: number[];
  daily30: number[];
  totalToday: number;
  totalWeek: number;
  total30Days: number;
  timeSynced: boolean;
  // Raw visit timestamps (epoch seconds, UTC), oldest -> newest. Day/night
  // classification happens client-side against each timestamp's local
  // browser hour - the device's clock is NTP/UTC only, so this is the one
  // correct place to apply the viewer's own timezone.
  visitTimes: number[];
  dayStartHour: number;
  dayEndHour: number;
}

// Mirrors useDeviceConfigApi's URL handling - the HTTP API lives on the same
// host as the WS endpoint.
function toHttpBase(deviceUrl: string): string {
  return deviceUrl.replace(/^ws/, "http").replace(/\/ws\/?$/, "");
}

// Visit counts change rarely (once per occupancy session) - a slow poll is
// plenty, no need to ride the WS telemetry stream for this.
const POLL_MS = 60_000;

export function useAnalytics(deviceUrl: string) {
  const base = toHttpBase(deviceUrl);
  const [data, setData] = useState<AnalyticsPayload | null>(null);
  const [error, setError] = useState<string | null>(null);

  const fetchAnalytics = useCallback(async () => {
    try {
      const res = await fetch(`${base}/analytics`);
      const json = (await res.json()) as AnalyticsPayload;
      setData(json);
      setError(null);
    } catch {
      setError("Couldn't load usage data from the device");
    }
  }, [base]);

  useEffect(() => {
    fetchAnalytics();
    const id = setInterval(fetchAnalytics, POLL_MS);
    return () => clearInterval(id);
  }, [fetchAnalytics]);

  return { data, error, refetch: fetchAnalytics };
}

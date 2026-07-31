import { useEffect, useRef, useState, useCallback } from "react";

export type Lr2State =
  | "homing"
  | "idle"
  | "cat_present"
  | "waiting"
  | "cycling"
  | "safety_stop"
  | "fault"
  | null;

export type ConnectionStatus =
  | "disconnected"
  | "connecting"
  | "connected"
  | "error";

export type Lr2Command = "cycle" | "reset_fault" | "drawer_emptied" | "resume";

export interface Lr2Telemetry {
  connectionStatus: ConnectionStatus;
  lastError: string | null;
  state: Lr2State;
  catPresent: boolean | null;
  cycleCount: number | null;
  drawerFull: boolean | null;
  drawerCycles: number | null;
  drawerThreshold: number | null;
  lastUpdateAt: number | null;
  uptimeSeconds: number | null;
  needsManualReset: boolean | null;
  catPresentWarning: boolean | null;
  ipAddress: string | null;
  rssi: number | null;
  firmwareBuild: string | null;
  sendCommand: (cmd: Lr2Command) => void;
}

interface Lr2StatePayload {
  state: Lr2State;
  catPresent: boolean;
  cycleCount: number;
  drawerFull: boolean;
  drawerCycles: number;
  drawerThreshold: number;
  uptimeSeconds: number;
  needsManualReset: boolean;
  catPresentWarning: boolean;
  ipAddress: string;
  rssi: number;
  firmwareBuild: string;
}

const RECONNECT_DELAY_MS = 3000;

// Talks directly to the ESP32's own WebSocket server (ws://lr2redux.local/ws)
// instead of an MQTT broker - the firmware is the only MQTT client now.
export function useLr2Socket(deviceUrl: string | null): Lr2Telemetry {
  const wsRef = useRef<WebSocket | null>(null);
  const reconnectTimer = useRef<number | null>(null);

  const [connectionStatus, setConnectionStatus] =
    useState<ConnectionStatus>("disconnected");
  const [lastError, setLastError] = useState<string | null>(null);
  const [payload, setPayload] = useState<Partial<Lr2StatePayload>>({});
  const [lastUpdateAt, setLastUpdateAt] = useState<number | null>(null);

  useEffect(() => {
    if (!deviceUrl) {
      setConnectionStatus("disconnected");
      return;
    }

    let cancelled = false;

    const connect = () => {
      if (cancelled) return;
      setConnectionStatus("connecting");

      const socket = new WebSocket(deviceUrl);
      wsRef.current = socket;

      socket.onopen = () => {
        setConnectionStatus("connected");
        setLastError(null);
      };

      socket.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data) as Partial<Lr2StatePayload>;
          setPayload((prev) => ({ ...prev, ...data }));
          setLastUpdateAt(Date.now());
        } catch {
          // ignore malformed frames
        }
      };

      socket.onerror = () => {
        setConnectionStatus("error");
        setLastError("WebSocket error");
      };

      socket.onclose = () => {
        wsRef.current = null;
        if (cancelled) return;
        setConnectionStatus("disconnected");
        reconnectTimer.current = window.setTimeout(connect, RECONNECT_DELAY_MS);
      };
    };

    connect();

    return () => {
      cancelled = true;
      if (reconnectTimer.current !== null) window.clearTimeout(reconnectTimer.current);
      wsRef.current?.close();
      wsRef.current = null;
    };
  }, [deviceUrl]);

  const sendCommand = useCallback((cmd: Lr2Command) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify({ cmd }));
    }
  }, []);

  return {
    connectionStatus,
    lastError,
    state: payload.state ?? null,
    catPresent: payload.catPresent ?? null,
    cycleCount: payload.cycleCount ?? null,
    drawerFull: payload.drawerFull ?? null,
    drawerCycles: payload.drawerCycles ?? null,
    drawerThreshold: payload.drawerThreshold ?? null,
    lastUpdateAt,
    uptimeSeconds: payload.uptimeSeconds ?? null,
    needsManualReset: payload.needsManualReset ?? null,
    catPresentWarning: payload.catPresentWarning ?? null,
    ipAddress: payload.ipAddress || null,
    rssi: connectionStatus === "connected" ? payload.rssi ?? null : null,
    firmwareBuild: payload.firmwareBuild || null,
    sendCommand,
  };
}

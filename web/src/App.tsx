import { useEffect, useState } from "react";
import {
  Box,
  Button,
  Card,
  CardBody,
  CardHeader,
  Grid,
  Heading,
  Meter,
  Text,
} from "grommet";
import { useLr2Socket } from "./hooks/useLr2Socket";
import { StatusPill } from "./components/StatusPill";
import { DeviceBar } from "./components/DeviceBar";
import { SettingsCard } from "./components/SettingsCard";

const STORAGE_KEY = "lr2redux.deviceUrl";
const UPDATE_STALE_MS = 15_000; // 3x the firmware's 5s broadcast interval

// When this app is served BY the board itself (the normal case - see
// web/package.json's build:device script), window.location already points
// at the device, so derive the WS URL from it instead of needing any config.
// Only fall back to the lr2redux.local guess when running off a dev server.
function deriveDefaultDeviceUrl(): string {
  if (import.meta.env.VITE_DEFAULT_DEVICE_URL) {
    return import.meta.env.VITE_DEFAULT_DEVICE_URL;
  }
  const { hostname, protocol, host } = window.location;
  const isDevServer = hostname === "localhost" || hostname === "127.0.0.1";
  if (!isDevServer) {
    const wsProtocol = protocol === "https:" ? "wss" : "ws";
    return `${wsProtocol}://${host}/ws`;
  }
  return "ws://lr2redux.local/ws";
}

function loadDeviceUrl(): string {
  return localStorage.getItem(STORAGE_KEY) ?? deriveDefaultDeviceUrl();
}

export default function App() {
  const [deviceUrl, setDeviceUrl] = useState<string>(loadDeviceUrl);
  const [now, setNow] = useState(() => Date.now());

  const telemetry = useLr2Socket(deviceUrl);
  const {
    connectionStatus,
    lastError,
    state,
    catPresent,
    cycleCount,
    drawerFull,
    drawerCycles,
    drawerThreshold,
    lastUpdateAt,
    uptimeSeconds,
    sendCommand,
  } = telemetry;

  useEffect(() => {
    if (connectionStatus !== "connected") return;
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [connectionStatus]);

  const handleDeviceSubmit = (url: string) => {
    localStorage.setItem(STORAGE_KEY, url);
    setDeviceUrl(url);
  };

  const updateAgeSeconds = lastUpdateAt
    ? Math.round((now - lastUpdateAt) / 1000)
    : null;
  const updateStale =
    updateAgeSeconds !== null && updateAgeSeconds * 1000 > UPDATE_STALE_MS;

  const canCycle = connectionStatus === "connected" && state === "idle";
  const canResetFault = connectionStatus === "connected" && state === "fault";

  return (
    <Box fill background="background" pad={{ horizontal: "medium", vertical: "medium" }}>
      <Box width={{ max: "960px" }} margin={{ horizontal: "auto" }} fill="horizontal" gap="medium">
        <Box>
          <Text
            size="xsmall"
            weight="bold"
            color="accent-1"
            style={{ letterSpacing: "0.14em", textTransform: "uppercase" }}
          >
            LR2-Redux dashboard
          </Text>
          <Heading level={1} margin={{ vertical: "xsmall" }} size="small">
            Litter Robot 2
          </Heading>
        </Box>

        <DeviceBar
          value={deviceUrl}
          onSubmit={handleDeviceSubmit}
          status={connectionStatus}
          error={lastError}
        />

        <Grid columns={{ count: "fit", size: "medium" }} gap="medium">
          <Card>
            <CardHeader pad="medium">
              <Text weight="bold">Status</Text>
            </CardHeader>
            <CardBody pad={{ horizontal: "medium", bottom: "medium" }} gap="medium">
              <StatusPill state={state} />
              <Box direction="row" justify="between">
                <Text color="text-weak" size="small">
                  Cat present
                </Text>
                <Text weight="bold">
                  {catPresent === null ? "—" : catPresent ? "Yes" : "No"}
                </Text>
              </Box>
              <Box direction="row" justify="between">
                <Text color="text-weak" size="small">
                  Cycle count
                </Text>
                <Text weight="bold">{cycleCount ?? "—"}</Text>
              </Box>
              <Box direction="row" justify="between">
                <Text color="text-weak" size="small">
                  Last update
                </Text>
                <Text weight="bold" color={updateStale ? "state-fault" : undefined}>
                  {updateAgeSeconds === null ? "—" : `${updateAgeSeconds}s ago`}
                </Text>
              </Box>
              <Box direction="row" justify="between">
                <Text color="text-weak" size="small">
                  Uptime
                </Text>
                <Text weight="bold">
                  {uptimeSeconds === null
                    ? "—"
                    : `${Math.floor(uptimeSeconds / 60)}m ${uptimeSeconds % 60}s`}
                </Text>
              </Box>
            </CardBody>
          </Card>

          <Card>
            <CardHeader pad="medium">
              <Text weight="bold">Drawer</Text>
            </CardHeader>
            <CardBody pad={{ horizontal: "medium", bottom: "medium" }} gap="medium">
              <Meter
                type="bar"
                round
                values={[
                  {
                    value: drawerCycles ?? 0,
                    color: drawerFull ? "state-fault" : "state-cycle",
                  },
                ]}
                max={drawerThreshold ?? 10}
              />
              <Box direction="row" justify="between">
                <Text color="text-weak" size="small">
                  Cycles since emptied
                </Text>
                <Text weight="bold">
                  {drawerCycles ?? "—"} / {drawerThreshold ?? "—"}
                </Text>
              </Box>
              <Box direction="row" justify="between" align="center">
                <Text color="text-weak" size="small">
                  Drawer full
                </Text>
                <Text weight="bold" color={drawerFull ? "state-fault" : undefined}>
                  {drawerFull === null ? "—" : drawerFull ? "Yes" : "No"}
                </Text>
              </Box>
              <Button
                label="Mark drawer emptied"
                onClick={() => sendCommand("drawer_emptied")}
                disabled={connectionStatus !== "connected"}
              />
            </CardBody>
          </Card>

          <Card>
            <CardHeader pad="medium">
              <Text weight="bold">Actions</Text>
            </CardHeader>
            <CardBody pad={{ horizontal: "medium", bottom: "medium" }} gap="small">
              <Button
                label="Cycle now"
                primary
                onClick={() => sendCommand("cycle")}
                disabled={!canCycle}
              />
              <Text size="xsmall" color="text-weak">
                {state === "idle"
                  ? "Ready — the globe is at Home."
                  : "Only available when the unit is idle."}
              </Text>

              <Button
                label="Reset fault"
                onClick={() => sendCommand("reset_fault")}
                disabled={!canResetFault}
              />
              <Text size="xsmall" color="text-weak">
                {state === "fault"
                  ? "Clears the fault and re-homes the globe."
                  : "Enabled when the unit reports a fault."}
              </Text>
            </CardBody>
          </Card>

          <SettingsCard deviceUrl={deviceUrl} />
        </Grid>
      </Box>
    </Box>
  );
}

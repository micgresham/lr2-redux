import { useEffect, useState } from "react";
import { Box, Button, Card, CardBody, CardHeader, CheckBox, Text, TextInput } from "grommet";
import { useDeviceConfigApi } from "../hooks/useDeviceConfigApi";

const MIN_WAIT_TIMER_MIN = 2;
const MIN_CAT_WARNING_MIN = 2;
const MIN_DRAWER_FULL_CYCLES = 1;
const MIN_HOME_OVERSHOOT_MS = 0;
const MAX_HOME_OVERSHOOT_MS = 15000;
const MIN_SHAKE_STEP_MS = 50;
const MAX_SHAKE_STEP_MS = 5000;
const MIN_SHAKE_COUNT = 0;
const MAX_SHAKE_COUNT = 20;

export function SettingsCard({ deviceUrl }: { deviceUrl: string }) {
  const { config, networks, scanning, saving, reconnecting, error, fetchConfig, scan, save } =
    useDeviceConfigApi(deviceUrl);

  const [wifiSsid, setWifiSsid] = useState("");
  const [wifiPass, setWifiPass] = useState("");
  const [mqttHost, setMqttHost] = useState("");
  const [mqttPort, setMqttPort] = useState("1883");
  const [mqttUser, setMqttUser] = useState("");
  const [mqttPass, setMqttPass] = useState("");
  const [waitTimerMin, setWaitTimerMin] = useState("7");
  const [requireManualReset, setRequireManualReset] = useState(false);
  const [catPresentWarningMin, setCatPresentWarningMin] = useState("2");
  const [drawerFullCycles, setDrawerFullCycles] = useState("10");
  const [homeOvershootMs, setHomeOvershootMs] = useState("3000");
  const [dumpShakeStepMs, setDumpShakeStepMs] = useState("400");
  const [dumpShakeCount, setDumpShakeCount] = useState("3");
  const [savedMsg, setSavedMsg] = useState<string | null>(null);
  const [validationError, setValidationError] = useState<string | null>(null);
  const [showAdvanced, setShowAdvanced] = useState(false);

  useEffect(() => {
    fetchConfig();
  }, [fetchConfig]);

  useEffect(() => {
    if (!config) return;
    setWifiSsid(config.wifiSsid ?? "");
    setMqttHost(config.mqttHost ?? "");
    setMqttPort(String(config.mqttPort ?? 1883));
    setMqttUser(config.mqttUser ?? "");
    setWaitTimerMin(String(config.waitTimerMin ?? 7));
    setRequireManualReset(config.requireManualReset ?? false);
    setCatPresentWarningMin(String(config.catPresentWarningMin ?? 2));
    setDrawerFullCycles(String(config.drawerFullCycles ?? 10));
    setHomeOvershootMs(String(config.homeOvershootMs ?? 3000));
    setDumpShakeStepMs(String(config.dumpShakeStepMs ?? 400));
    setDumpShakeCount(String(config.dumpShakeCount ?? 3));
  }, [config]);

  const handleSave = async () => {
    setSavedMsg(null);
    setValidationError(null);

    const waitTimerValue = parseInt(waitTimerMin, 10) || 0;
    if (waitTimerValue < MIN_WAIT_TIMER_MIN) {
      setValidationError(
        `Wait time can't be less than ${MIN_WAIT_TIMER_MIN} minutes — that's how long the cat gets to actually leave the globe before it cycles.`,
      );
      return;
    }

    const catWarningValue = parseInt(catPresentWarningMin, 10) || 0;
    if (catWarningValue < MIN_CAT_WARNING_MIN) {
      setValidationError(
        `Cat-present warning can't be less than ${MIN_CAT_WARNING_MIN} minutes.`,
      );
      return;
    }

    const drawerFullValue = parseInt(drawerFullCycles, 10) || 0;
    if (drawerFullValue < MIN_DRAWER_FULL_CYCLES) {
      setValidationError(
        `Cycles to drawer full can't be less than ${MIN_DRAWER_FULL_CYCLES}.`,
      );
      return;
    }

    const homeOvershootValue = parseInt(homeOvershootMs, 10) || 0;
    if (homeOvershootValue < MIN_HOME_OVERSHOOT_MS || homeOvershootValue > MAX_HOME_OVERSHOOT_MS) {
      setValidationError(
        `Home overshoot must be between ${MIN_HOME_OVERSHOOT_MS} and ${MAX_HOME_OVERSHOOT_MS} ms.`,
      );
      return;
    }

    const dumpShakeStepValue = parseInt(dumpShakeStepMs, 10) || 0;
    if (dumpShakeStepValue < MIN_SHAKE_STEP_MS || dumpShakeStepValue > MAX_SHAKE_STEP_MS) {
      setValidationError(
        `Shake swing duration must be between ${MIN_SHAKE_STEP_MS} and ${MAX_SHAKE_STEP_MS} ms.`,
      );
      return;
    }

    const dumpShakeCountValue = parseInt(dumpShakeCount, 10) || 0;
    if (dumpShakeCountValue < MIN_SHAKE_COUNT || dumpShakeCountValue > MAX_SHAKE_COUNT) {
      setValidationError(
        `Number of shakes must be between ${MIN_SHAKE_COUNT} and ${MAX_SHAKE_COUNT}.`,
      );
      return;
    }

    const changingWifi = config !== null && wifiSsid !== config.wifiSsid;
    const ok = await save(
      {
        wifiSsid,
        wifiPass,
        mqttHost,
        mqttPort: parseInt(mqttPort, 10) || 1883,
        mqttUser,
        mqttPass,
        waitTimerMin: waitTimerValue,
        requireManualReset,
        catPresentWarningMin: catWarningValue,
        drawerFullCycles: drawerFullValue,
        homeOvershootMs: homeOvershootValue,
        dumpShakeStepMs: dumpShakeStepValue,
        dumpShakeCount: dumpShakeCountValue,
      },
      // Changing the SSID reboots the device onto a different network -
      // polling this same URL afterward would never succeed.
      { skipReconnectPoll: changingWifi },
    );
    if (ok) {
      setSavedMsg(
        changingWifi
          ? "Saved. Rebooting onto the new network — reconnect the dashboard there."
          : "Saved and reconnected.",
      );
    }
  };

  return (
    <Card>
      <CardHeader pad="medium">
        <Text weight="bold">Settings</Text>
      </CardHeader>
      <CardBody pad={{ horizontal: "medium", bottom: "medium" }} gap="small">
        <Box gap="xsmall">
          <Text size="small" color="text-weak">
            WiFi network
          </Text>
          <TextInput
            value={wifiSsid}
            onChange={(e) => setWifiSsid(e.target.value)}
            placeholder="SSID"
          />
          <Button
            label={scanning ? "Scanning…" : "Scan for networks"}
            onClick={() => scan()}
            disabled={scanning}
            size="small"
          />
          {networks.length > 0 && (
            <Box
              border
              round="xsmall"
              pad="xsmall"
              gap="xxsmall"
              overflow={{ vertical: "auto" }}
              height={{ max: "140px" }}
            >
              {networks.map((n) => (
                <Box
                  key={n.ssid}
                  direction="row"
                  justify="between"
                  pad="xsmall"
                  round="xsmall"
                  onClick={() => setWifiSsid(n.ssid)}
                  style={{ cursor: "pointer" }}
                  background={n.ssid === wifiSsid ? "background-contrast" : undefined}
                >
                  <Text size="small">{n.ssid}</Text>
                  <Text size="small" color="text-weak">
                    {n.rssi} dBm{n.secure ? " 🔒" : ""}
                  </Text>
                </Box>
              ))}
            </Box>
          )}
        </Box>

        <Box gap="xsmall">
          <Text size="small" color="text-weak">
            WiFi password
          </Text>
          <TextInput
            type="password"
            value={wifiPass}
            onChange={(e) => setWifiPass(e.target.value)}
            placeholder="leave blank to keep current"
          />
        </Box>

        <Box gap="xsmall">
          <Text size="small" color="text-weak">
            MQTT host
          </Text>
          <TextInput
            value={mqttHost}
            onChange={(e) => setMqttHost(e.target.value)}
            placeholder="192.168.1.10 (optional)"
          />
        </Box>

        <Box gap="xsmall">
          <Text size="small" color="text-weak">
            MQTT port
          </Text>
          <TextInput
            type="number"
            value={mqttPort}
            onChange={(e) => setMqttPort(e.target.value)}
          />
        </Box>

        <Box gap="xsmall">
          <Text size="small" color="text-weak">
            MQTT username
          </Text>
          <TextInput
            value={mqttUser}
            onChange={(e) => setMqttUser(e.target.value)}
            placeholder="optional"
          />
        </Box>

        <Box gap="xsmall">
          <Text size="small" color="text-weak">
            MQTT password
          </Text>
          <TextInput
            type="password"
            value={mqttPass}
            onChange={(e) => setMqttPass(e.target.value)}
            placeholder="leave blank to keep current"
          />
        </Box>

        <Button
          label={showAdvanced ? "Hide advanced settings" : "Show advanced settings"}
          onClick={() => setShowAdvanced((v) => !v)}
          size="small"
        />

        {showAdvanced && (
          <Box gap="xsmall" pad="small" round="xsmall" border background="background-contrast">
            <Text size="small" color="text-weak">
              Wait time (min) — how long the cat gets to leave before a cycle starts
            </Text>
            <TextInput
              type="number"
              min={MIN_WAIT_TIMER_MIN}
              value={waitTimerMin}
              onChange={(e) => setWaitTimerMin(e.target.value)}
            />
            <Text size="xsmall" color="text-weak">
              Minimum {MIN_WAIT_TIMER_MIN} minutes — enforced by the device
              regardless of what's entered here.
            </Text>

            <Text size="small" color="text-weak" margin={{ top: "small" }}>
              Cat-present warning (min) — flags unusually long occupancy
            </Text>
            <TextInput
              type="number"
              min={MIN_CAT_WARNING_MIN}
              value={catPresentWarningMin}
              onChange={(e) => setCatPresentWarningMin(e.target.value)}
            />
            <Text size="xsmall" color="text-weak">
              Minimum {MIN_CAT_WARNING_MIN} minutes, matching the original
              stock board — can be set higher, not lower.
            </Text>

            <Box direction="row" align="center" gap="small" margin={{ top: "small" }}>
              <CheckBox
                checked={requireManualReset}
                onChange={(e) => setRequireManualReset(e.target.checked)}
                label="Require manual reset after any interruption"
              />
            </Box>
            <Text size="xsmall" color="text-weak">
              Off (default): only a pinch detection needs manual
              confirmation to resume — a weight-switch interruption
              auto-resumes on its own after a 2-minute cooldown. On: matches
              the original stock board exactly — every interruption needs a
              button press or dashboard confirmation, nothing auto-resumes.
            </Text>

            <Text size="small" color="text-weak" margin={{ top: "small" }}>
              Cycles until drawer full
            </Text>
            <TextInput
              type="number"
              min={MIN_DRAWER_FULL_CYCLES}
              value={drawerFullCycles}
              onChange={(e) => setDrawerFullCycles(e.target.value)}
            />
            <Text size="xsmall" color="text-weak">
              Defaults to 10 — tune it based on your own experience (litter
              type and how many cats use it both affect how many cycles the
              drawer actually holds before it needs emptying).
            </Text>

            <Text size="small" color="text-weak" margin={{ top: "small" }}>
              Home overshoot (ms) — how far past Home it reverses before
              coming back to settle, to help level litter
            </Text>
            <TextInput
              type="number"
              min={MIN_HOME_OVERSHOOT_MS}
              max={MAX_HOME_OVERSHOOT_MS}
              value={homeOvershootMs}
              onChange={(e) => setHomeOvershootMs(e.target.value)}
            />
            <Text size="xsmall" color="text-weak">
              There's no rotation sensor for this, so distance is
              approximated by time at a fixed motor speed — more ms means
              more rotation. Defaults to 3000ms. Increase for litter that
              needs more spreading, decrease if it's dragging the globe too
              far. Range {MIN_HOME_OVERSHOOT_MS}–{MAX_HOME_OVERSHOOT_MS}ms.
            </Text>

            <Text size="small" color="text-weak" margin={{ top: "small" }}>
              Shake swing duration (ms) — how long each back-and-forth swing
              lasts when dislodging stuck clumps at the dump position
            </Text>
            <TextInput
              type="number"
              min={MIN_SHAKE_STEP_MS}
              max={MAX_SHAKE_STEP_MS}
              value={dumpShakeStepMs}
              onChange={(e) => setDumpShakeStepMs(e.target.value)}
            />
            <Text size="xsmall" color="text-weak">
              Same time-as-distance caveat as home overshoot — a longer
              swing rotates further at the same motor speed. Defaults to
              400ms. Range {MIN_SHAKE_STEP_MS}–{MAX_SHAKE_STEP_MS}ms.
            </Text>

            <Text size="small" color="text-weak" margin={{ top: "small" }}>
              Number of shakes
            </Text>
            <TextInput
              type="number"
              min={MIN_SHAKE_COUNT}
              max={MAX_SHAKE_COUNT}
              value={dumpShakeCount}
              onChange={(e) => setDumpShakeCount(e.target.value)}
            />
            <Text size="xsmall" color="text-weak">
              How many full back-and-forth swings to do. Defaults to 3; set
              to 0 to skip the shake entirely. Range {MIN_SHAKE_COUNT}–
              {MAX_SHAKE_COUNT}.
            </Text>
          </Box>
        )}

        <Button
          label={saving ? "Saving…" : reconnecting ? "Reconnecting…" : "Save & reboot"}
          primary
          onClick={handleSave}
          disabled={saving || reconnecting}
        />
        {validationError && (
          <Text size="xsmall" color="state-fault">
            {validationError}
          </Text>
        )}
        {error && (
          <Text size="xsmall" color="state-fault">
            {error}
          </Text>
        )}
        {savedMsg && (
          <Text size="xsmall" color="state-idle">
            {savedMsg}
          </Text>
        )}
      </CardBody>
    </Card>
  );
}

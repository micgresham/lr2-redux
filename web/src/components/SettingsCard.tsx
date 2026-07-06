import { useEffect, useState } from "react";
import { Box, Button, Card, CardBody, CardHeader, CheckBox, Text, TextInput } from "grommet";
import { useDeviceConfigApi } from "../hooks/useDeviceConfigApi";

const MIN_WAIT_TIMER_MIN = 2;
const MIN_CAT_WARNING_MIN = 2;
const MIN_DRAWER_FULL_CYCLES = 1;

export function SettingsCard({ deviceUrl }: { deviceUrl: string }) {
  const { config, networks, scanning, saving, error, fetchConfig, scan, save } =
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

    const changingWifi = config !== null && wifiSsid !== config.wifiSsid;
    const ok = await save({
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
    });
    if (ok) {
      setSavedMsg(
        changingWifi
          ? "Saved. Rebooting onto the new network — reconnect the dashboard there."
          : "Saved. The board is rebooting.",
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
          </Box>
        )}

        <Button
          label={saving ? "Saving…" : "Save & reboot"}
          primary
          onClick={handleSave}
          disabled={saving}
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

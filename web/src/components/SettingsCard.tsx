import { useEffect, useState } from "react";
import { Box, Button, Card, CardBody, CardHeader, Text, TextInput } from "grommet";
import { useDeviceConfigApi } from "../hooks/useDeviceConfigApi";

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
  const [savedMsg, setSavedMsg] = useState<string | null>(null);

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
  }, [config]);

  const handleSave = async () => {
    setSavedMsg(null);
    const changingWifi = config !== null && wifiSsid !== config.wifiSsid;
    const ok = await save({
      wifiSsid,
      wifiPass,
      mqttHost,
      mqttPort: parseInt(mqttPort, 10) || 1883,
      mqttUser,
      mqttPass,
      waitTimerMin: parseInt(waitTimerMin, 10) || 7,
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

        <Box direction="row" gap="small">
          <Box flex gap="xsmall">
            <Text size="small" color="text-weak">
              MQTT port
            </Text>
            <TextInput
              type="number"
              value={mqttPort}
              onChange={(e) => setMqttPort(e.target.value)}
            />
          </Box>
          <Box flex gap="xsmall">
            <Text size="small" color="text-weak">
              Wait time (min)
            </Text>
            <TextInput
              type="number"
              value={waitTimerMin}
              onChange={(e) => setWaitTimerMin(e.target.value)}
            />
          </Box>
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
          label={saving ? "Saving…" : "Save & reboot"}
          primary
          onClick={handleSave}
          disabled={saving}
        />
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

import { useState } from "react";
import {
  Box,
  Button,
  Card,
  CardBody,
  CardHeader,
  FileInput,
  Meter,
  Text,
  TextInput,
} from "grommet";

// Mirrors the other hooks' URL handling - the HTTP API lives on the same
// host as the WS endpoint.
function toHttpBase(deviceUrl: string): string {
  return deviceUrl.replace(/^ws/, "http").replace(/\/ws\/?$/, "");
}

export function FirmwareUpdateCard({ deviceUrl }: { deviceUrl: string }) {
  const base = toHttpBase(deviceUrl);
  const [file, setFile] = useState<File | null>(null);
  const [password, setPassword] = useState("");
  const [progress, setProgress] = useState<number | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const handleUpload = () => {
    if (!file) return;
    setError(null);
    setMessage(null);
    setProgress(0);

    const formData = new FormData();
    formData.append("firmware", file);

    // XMLHttpRequest rather than fetch specifically for upload progress
    // events, which fetch doesn't expose.
    const xhr = new XMLHttpRequest();
    xhr.open("POST", `${base}/update`);
    xhr.setRequestHeader("X-OTA-Password", password);
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) setProgress(Math.round((e.loaded / e.total) * 100));
    };
    xhr.onload = () => {
      setProgress(null);
      if (xhr.status === 200) {
        setMessage("Update accepted — the board is rebooting onto the new firmware.");
      } else if (xhr.status === 401) {
        setError("Wrong OTA password.");
      } else {
        setError(xhr.responseText || `Update failed (status ${xhr.status})`);
      }
    };
    xhr.onerror = () => {
      setProgress(null);
      setError("Upload failed — is the device reachable?");
    };
    xhr.send(formData);
  };

  return (
    <Card>
      <CardHeader pad="medium">
        <Text weight="bold">Firmware update</Text>
      </CardHeader>
      <CardBody pad={{ horizontal: "medium", bottom: "medium" }} gap="small">
        <FileInput
          name="firmware"
          onChange={(_event, { files }) => setFile(files?.[0] ?? null)}
        />
        <Box gap="xsmall">
          <Text size="small" color="text-weak">
            OTA password
          </Text>
          <TextInput
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            placeholder="from secrets.h"
          />
        </Box>
        <Button
          label={progress !== null ? `Uploading… ${progress}%` : "Upload & flash"}
          primary
          onClick={handleUpload}
          disabled={!file || !password || progress !== null}
        />
        {progress !== null && <Meter type="bar" value={progress} thickness="small" />}
        {message && (
          <Text size="xsmall" color="state-idle">
            {message}
          </Text>
        )}
        {error && (
          <Text size="xsmall" color="state-fault">
            {error}
          </Text>
        )}
      </CardBody>
    </Card>
  );
}

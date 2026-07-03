import { Box, Button, Text, TextInput } from "grommet";
import { useState } from "react";
import { ConnectionStatus } from "../hooks/useLr2Socket";

const STATUS_META: Record<
  ConnectionStatus,
  { label: string; color: string }
> = {
  connected: { label: "Connected", color: "state-idle" },
  connecting: { label: "Connecting…", color: "state-wait" },
  disconnected: { label: "Disconnected", color: "text-weak" },
  error: { label: "Connection error", color: "state-fault" },
};

export function DeviceBar({
  value,
  onSubmit,
  status,
  error,
}: {
  value: string;
  onSubmit: (url: string) => void;
  status: ConnectionStatus;
  error: string | null;
}) {
  const [draft, setDraft] = useState(value);
  const meta = STATUS_META[status];

  return (
    <Box
      direction="row"
      align="center"
      justify="between"
      wrap
      gap="small"
      pad={{ vertical: "small" }}
    >
      <Box direction="row" align="center" gap="small" flex="grow">
        <TextInput
          value={draft}
          onChange={(e) => setDraft(e.target.value)}
          placeholder="ws://lr2redux.local/ws"
          size="small"
        />
        <Button
          label="Connect"
          size="small"
          primary
          onClick={() => onSubmit(draft)}
        />
      </Box>
      <Box direction="row" align="center" gap="xsmall">
        <Box width="8px" height="8px" round="full" background={meta.color} />
        <Text size="small" color="text-weak">
          {meta.label}
        </Text>
      </Box>
      {error && (
        <Text size="xsmall" color="state-fault">
          {error}
        </Text>
      )}
    </Box>
  );
}

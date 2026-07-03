import { Box, Text } from "grommet";
import { Lr2State } from "../hooks/useLr2Socket";

const STATE_META: Record<
  NonNullable<Lr2State>,
  { label: string; color: string; description: string }
> = {
  homing: {
    label: "Homing",
    color: "state-cycle",
    description: "Finding the Home sensor after power-on",
  },
  idle: {
    label: "Idle",
    color: "state-idle",
    description: "At rest, waiting for a cat",
  },
  cat_present: {
    label: "Cat present",
    color: "state-wait",
    description: "Weight switch engaged",
  },
  waiting: {
    label: "Waiting",
    color: "state-wait",
    description: "Cat left — clean-cycle timer running",
  },
  cycling: {
    label: "Cycling",
    color: "state-cycle",
    description: "Globe rotating through the cycle",
  },
  safety_stop: {
    label: "Safety stop",
    color: "state-wait",
    description: "Weight switch tripped mid-cycle — motor halted",
  },
  fault: {
    label: "Fault",
    color: "state-fault",
    description: "A cycle segment timed out — needs reset_fault",
  },
};

export function StatusPill({ state }: { state: Lr2State }) {
  if (!state) {
    return (
      <Box direction="row" align="center" gap="xsmall">
        <Box
          width="10px"
          height="10px"
          round="full"
          background="text-weak"
        />
        <Text weight="bold" color="text-weak">
          Unknown
        </Text>
      </Box>
    );
  }

  const meta = STATE_META[state];
  return (
    <Box gap="xxsmall">
      <Box direction="row" align="center" gap="xsmall">
        <Box width="10px" height="10px" round="full" background={meta.color} />
        <Text weight="bold" size="large">
          {meta.label}
        </Text>
      </Box>
      <Text size="small" color="text-weak">
        {meta.description}
      </Text>
    </Box>
  );
}

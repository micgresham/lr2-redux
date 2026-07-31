import { useMemo } from "react";
import { Box, Card, CardBody, CardHeader, Text } from "grommet";
import { useAnalytics } from "../hooks/useAnalytics";
import { useDailyVisitBuckets } from "../hooks/useDailyVisitBuckets";
import { VisitChart } from "./VisitChart";

export function UsageCard({ deviceUrl }: { deviceUrl: string }) {
  const { data, error } = useAnalytics(deviceUrl);

  const visitTimes = useMemo(() => data?.visitTimes ?? [], [data]);
  const { totalToday, totalWeek, total30Days, daily30 } = useDailyVisitBuckets(visitTimes);

  return (
    <Card>
      <CardHeader pad="medium">
        <Text weight="bold">Usage</Text>
      </CardHeader>
      <CardBody pad={{ horizontal: "medium", bottom: "medium" }} gap="medium">
        <Box direction="row" justify="between">
          <Box>
            <Text size="xsmall" color="text-weak">
              Today
            </Text>
            <Text weight="bold" size="large">
              {data ? totalToday : "—"}
            </Text>
          </Box>
          <Box>
            <Text size="xsmall" color="text-weak">
              This week
            </Text>
            <Text weight="bold" size="large">
              {data ? totalWeek : "—"}
            </Text>
          </Box>
          <Box>
            <Text size="xsmall" color="text-weak">
              Last 30 days
            </Text>
            <Text weight="bold" size="large">
              {data ? total30Days : "—"}
            </Text>
          </Box>
        </Box>

        {data && <VisitChart daily30={daily30} />}

        {data && !data.timeSynced && (
          <Text size="xsmall" color="state-wait">
            Waiting for the device's clock to sync (NTP) — visit tracking starts
            once it does.
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

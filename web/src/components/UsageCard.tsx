import { useMemo, useState } from "react";
import { Box, Card, CardBody, CardHeader, Text } from "grommet";
import { useAnalytics } from "../hooks/useAnalytics";

const CHART_HEIGHT = 64;
const DAY_MS = 24 * 60 * 60 * 1000;

function dayLabel(daysAgo: number): string {
  if (daysAgo === 0) return "Today";
  if (daysAgo === 1) return "Yesterday";
  return `${daysAgo} days ago`;
}

// Midnight of the viewer's own local calendar day containing `ms` - the
// device's clock is deliberately UTC-only (see main.cpp's NTP setup), so
// "today" has to be computed here, against the browser's timezone, not
// trusted from the device (which can only offer a rolling 24h window with
// no timezone concept at all - that's exactly the bug this replaces: a
// visit from last night was showing up as "today" whenever the current
// UTC hour hadn't rolled over a full 24h since it happened, and even a
// UTC calendar-day boundary would still be wrong for a non-UTC viewer).
function localDayStart(ms: number): number {
  const d = new Date(ms);
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

export function UsageCard({ deviceUrl }: { deviceUrl: string }) {
  const { data, error } = useAnalytics(deviceUrl);
  const [hoverIndex, setHoverIndex] = useState<number | null>(null);

  const visitTimes = useMemo(() => data?.visitTimes ?? [], [data]);

  const { totalToday, totalWeek, total30Days, daily30 } = useMemo(() => {
    const todayStart = localDayStart(Date.now());
    const buckets = new Array(30).fill(0); // index 0 = 29 days ago ... 29 = today
    let today = 0;
    let week = 0;
    let all30 = 0;

    for (const epochSec of visitTimes) {
      const daysAgo = Math.round((todayStart - localDayStart(epochSec * 1000)) / DAY_MS);
      if (daysAgo < 0 || daysAgo >= 30) continue; // outside the 30-day window (or clock skew)
      all30++;
      buckets[29 - daysAgo]++;
      if (daysAgo < 7) week++;
      if (daysAgo === 0) today++;
    }

    return { totalToday: today, totalWeek: week, total30Days: all30, daily30: buckets };
  }, [visitTimes]);

  const max = Math.max(1, ...daily30);
  const hovered = hoverIndex !== null ? daily30[hoverIndex] : null;
  const hoveredDaysAgo = hoverIndex !== null ? daily30.length - 1 - hoverIndex : null;

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

        {data && (
          <Box>
            <Box
              height={`${CHART_HEIGHT}px`}
              direction="row"
              align="end"
              gap="2px"
              aria-label="Visits per day, last 30 days"
            >
              {daily30.map((count, i) => {
                const daysAgo = daily30.length - 1 - i;
                const heightPct = Math.max(4, (count / max) * 100);
                const isHovered = hoverIndex === i;
                return (
                  <Box
                    key={i}
                    flex
                    height={`${heightPct}%`}
                    background={isHovered ? "accent-1" : "state-cycle"}
                    round={{ corner: "top", size: "3px" }}
                    style={{ minWidth: "2px", cursor: "default", opacity: isHovered ? 1 : 0.75 }}
                    title={`${dayLabel(daysAgo)}: ${count} visit${count === 1 ? "" : "s"}`}
                    onMouseEnter={() => setHoverIndex(i)}
                    onMouseLeave={() => setHoverIndex(null)}
                  />
                );
              })}
            </Box>
            <Box direction="row" justify="between" margin={{ top: "xxsmall" }}>
              <Text size="xsmall" color="text-weak">
                30 days ago
              </Text>
              <Text size="xsmall" color="text-weak">
                Today
              </Text>
            </Box>
            <Box height="20px" justify="center">
              <Text size="xsmall" weight="bold">
                {hovered !== null && hoveredDaysAgo !== null
                  ? `${dayLabel(hoveredDaysAgo)}: ${hovered} visit${hovered === 1 ? "" : "s"}`
                  : ""}
              </Text>
            </Box>
          </Box>
        )}

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

import { useEffect, useMemo, useState } from "react";
import { Box, Button, Card, CardBody, CardHeader, Select, Text } from "grommet";
import { useAnalytics } from "../hooks/useAnalytics";
import { useDeviceConfigApi } from "../hooks/useDeviceConfigApi";
import { useDailyVisitBuckets } from "../hooks/useDailyVisitBuckets";
import { VisitChart } from "./VisitChart";

const HOUR_LABELS = Array.from({ length: 24 }, (_, h) => formatHour(h));
const RECENT_VISITS_SHOWN = 50;

function formatHour(h: number): string {
  const period = h < 12 ? "AM" : "PM";
  const displayHour = h % 12 === 0 ? 12 : h % 12;
  return `${displayHour}${period}`;
}

// Classification happens against each timestamp's LOCAL browser hour - the
// device's clock is deliberately UTC-only (see main.cpp's NTP setup), so
// this is the one place that should ever apply the viewer's own timezone.
function isDayHour(epochSec: number, dayStartHour: number, dayEndHour: number): boolean {
  const hour = new Date(epochSec * 1000).getHours();
  if (dayStartHour === dayEndHour) return true; // degenerate config - treat everything as "day"
  if (dayStartHour < dayEndHour) return hour >= dayStartHour && hour < dayEndHour;
  return hour >= dayStartHour || hour < dayEndHour; // window wraps midnight
}

function averageGapSeconds(sortedTimes: number[]): number | null {
  if (sortedTimes.length < 2) return null;
  let total = 0;
  for (let i = 1; i < sortedTimes.length; i++) total += sortedTimes[i] - sortedTimes[i - 1];
  return total / (sortedTimes.length - 1);
}

function formatDuration(seconds: number): string {
  const totalMinutes = Math.round(seconds / 60);
  const days = Math.floor(totalMinutes / 1440);
  const hours = Math.floor((totalMinutes % 1440) / 60);
  const minutes = totalMinutes % 60;
  const parts: string[] = [];
  if (days > 0) parts.push(`${days}d`);
  if (hours > 0) parts.push(`${hours}h`);
  if (minutes > 0 || parts.length === 0) parts.push(`${minutes}m`);
  return parts.join(" ");
}

export function AnalyticsPage({ deviceUrl }: { deviceUrl: string }) {
  const { data, error } = useAnalytics(deviceUrl);
  const { save, saving, reconnecting, error: saveError } = useDeviceConfigApi(deviceUrl);

  const [dayStartHour, setDayStartHour] = useState(6);
  const [dayEndHour, setDayEndHour] = useState(20);
  const [savedMsg, setSavedMsg] = useState<string | null>(null);

  // Hydrate the editable hours from the device once, same pattern as
  // SettingsCard - after that the user's in-progress edit shouldn't get
  // clobbered by the next 60s analytics poll.
  const [hydrated, setHydrated] = useState(false);
  useEffect(() => {
    if (!data || hydrated) return;
    setDayStartHour(data.dayStartHour);
    setDayEndHour(data.dayEndHour);
    setHydrated(true);
  }, [data, hydrated]);

  const visitTimes = useMemo(() => data?.visitTimes ?? [], [data]);
  const { daily30 } = useDailyVisitBuckets(visitTimes);

  const { dayAvgSec, nightAvgSec, dayCount, nightCount } = useMemo(() => {
    const sorted = [...visitTimes].sort((a, b) => a - b);
    const dayTimes = sorted.filter((t) => isDayHour(t, dayStartHour, dayEndHour));
    const nightTimes = sorted.filter((t) => !isDayHour(t, dayStartHour, dayEndHour));
    return {
      dayAvgSec: averageGapSeconds(dayTimes),
      nightAvgSec: averageGapSeconds(nightTimes),
      dayCount: dayTimes.length,
      nightCount: nightTimes.length,
    };
  }, [visitTimes, dayStartHour, dayEndHour]);

  const recentVisits = useMemo(
    () => [...visitTimes].sort((a, b) => b - a).slice(0, RECENT_VISITS_SHOWN),
    [visitTimes],
  );

  const handleSaveHours = async () => {
    setSavedMsg(null);
    const ok = await save({ dayStartHour, dayEndHour });
    if (ok) setSavedMsg("Saved and reconnected.");
  };

  const handleExportCsv = () => {
    const rows = [...visitTimes]
      .sort((a, b) => a - b)
      .map((t) => {
        const dayOrNight = isDayHour(t, dayStartHour, dayEndHour) ? "Day" : "Night";
        return `${t},${new Date(t * 1000).toISOString()},${dayOrNight}`;
      });
    const csv = ["epochSeconds,timestampUTC,dayOrNight", ...rows].join("\n");
    const blob = new Blob([csv], { type: "text/csv" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `lr2redux-visits-${new Date().toISOString().slice(0, 10)}.csv`;
    a.click();
    URL.revokeObjectURL(url);
  };

  return (
    <Box gap="medium">
      <Card>
        <CardHeader pad="medium">
          <Text weight="bold">Day / night hours</Text>
        </CardHeader>
        <CardBody pad={{ horizontal: "medium", bottom: "medium" }} gap="small">
          <Text size="small" color="text-weak">
            Used below to split visit intervals into day vs. night, based on
            each visit's local time in your own browser (not the device's
            clock, which stays UTC internally).
          </Text>
          <Box direction="row" gap="medium">
            <Box gap="xsmall" width="small">
              <Text size="small" color="text-weak">
                Day starts
              </Text>
              <Select
                options={HOUR_LABELS}
                value={formatHour(dayStartHour)}
                onChange={({ option }) => setDayStartHour(HOUR_LABELS.indexOf(option))}
              />
            </Box>
            <Box gap="xsmall" width="small">
              <Text size="small" color="text-weak">
                Day ends
              </Text>
              <Select
                options={HOUR_LABELS}
                value={formatHour(dayEndHour)}
                onChange={({ option }) => setDayEndHour(HOUR_LABELS.indexOf(option))}
              />
            </Box>
          </Box>
          <Button
            label={saving ? "Saving…" : reconnecting ? "Reconnecting…" : "Save & reboot"}
            primary
            onClick={handleSaveHours}
            disabled={saving || reconnecting}
            alignSelf="start"
          />
          {saveError && (
            <Text size="xsmall" color="state-fault">
              {saveError}
            </Text>
          )}
          {savedMsg && (
            <Text size="xsmall" color="state-idle">
              {savedMsg}
            </Text>
          )}
        </CardBody>
      </Card>

      <Card>
        <CardHeader pad="medium">
          <Text weight="bold">Average time between visits</Text>
        </CardHeader>
        <CardBody pad={{ horizontal: "medium", bottom: "medium" }} gap="medium">
          <Box direction="row" gap="large">
            <Box gap="xxsmall">
              <Text size="xsmall" color="text-weak">
                Day ({formatHour(dayStartHour)}–{formatHour(dayEndHour)})
              </Text>
              <Text weight="bold" size="large">
                {dayAvgSec !== null ? formatDuration(dayAvgSec) : "—"}
              </Text>
              <Text size="xsmall" color="text-weak">
                {dayCount} visit{dayCount === 1 ? "" : "s"}
              </Text>
            </Box>
            <Box gap="xxsmall">
              <Text size="xsmall" color="text-weak">
                Night ({formatHour(dayEndHour)}–{formatHour(dayStartHour)})
              </Text>
              <Text weight="bold" size="large">
                {nightAvgSec !== null ? formatDuration(nightAvgSec) : "—"}
              </Text>
              <Text size="xsmall" color="text-weak">
                {nightCount} visit{nightCount === 1 ? "" : "s"}
              </Text>
            </Box>
          </Box>
          {visitTimes.length > 0 && visitTimes.length < 4 && (
            <Text size="xsmall" color="text-weak">
              Averages need at least a couple of visits in each bucket to
              mean much — check back after a few more.
            </Text>
          )}
          {data && !data.timeSynced && (
            <Text size="xsmall" color="state-wait">
              Waiting for the device's clock to sync (NTP) — visit tracking
              starts once it does.
            </Text>
          )}
        </CardBody>
      </Card>

      <Card>
        <CardHeader pad="medium">
          <Text weight="bold">Visits per day</Text>
        </CardHeader>
        <CardBody pad={{ horizontal: "medium", bottom: "medium" }}>
          {data ? (
            <VisitChart daily30={daily30} />
          ) : (
            <Text size="small" color="text-weak">
              No data yet.
            </Text>
          )}
        </CardBody>
      </Card>

      <Card>
        <CardHeader pad="medium" justify="between">
          <Text weight="bold">Recent visits</Text>
          <Button
            label="Export CSV"
            size="small"
            onClick={handleExportCsv}
            disabled={visitTimes.length === 0}
          />
        </CardHeader>
        <CardBody pad={{ horizontal: "medium", bottom: "medium" }} gap="small">
          {recentVisits.length === 0 ? (
            <Text size="small" color="text-weak">
              No visits recorded yet.
            </Text>
          ) : (
            <>
              <Box overflow={{ vertical: "auto" }} height={{ max: "320px" }} gap="xxsmall">
                {recentVisits.map((t) => {
                  const isDay = isDayHour(t, dayStartHour, dayEndHour);
                  return (
                    <Box
                      key={t}
                      direction="row"
                      justify="between"
                      align="center"
                      pad={{ vertical: "xxsmall" }}
                    >
                      <Text size="small">{new Date(t * 1000).toLocaleString()}</Text>
                      <Text size="xsmall" color={isDay ? "state-cycle" : "accent-1"}>
                        {isDay ? "Day" : "Night"}
                      </Text>
                    </Box>
                  );
                })}
              </Box>
              <Text size="xsmall" color="text-weak">
                Showing latest {recentVisits.length} of {visitTimes.length} stored (up to
                300). CSV export includes all {visitTimes.length}.
              </Text>
            </>
          )}
        </CardBody>
      </Card>

      {error && (
        <Text size="xsmall" color="state-fault">
          {error}
        </Text>
      )}
    </Box>
  );
}

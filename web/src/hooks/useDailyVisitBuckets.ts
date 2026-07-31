import { useMemo } from "react";

const DAY_MS = 24 * 60 * 60 * 1000;

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

// Shared by UsageCard (Dashboard tab) and AnalyticsPage (Analytics tab) -
// both show the same 30-day visit chart, just alongside different other
// data, so the bucketing logic lives here once instead of twice.
export function useDailyVisitBuckets(visitTimes: number[]) {
  return useMemo(() => {
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
}

import { useState } from "react";
import { Box, Text } from "grommet";

const CHART_HEIGHT = 64;

function dayLabel(daysAgo: number): string {
  if (daysAgo === 0) return "Today";
  if (daysAgo === 1) return "Yesterday";
  return `${daysAgo} days ago`;
}

// The 30-day visit bar chart, shared between UsageCard (Dashboard tab) and
// AnalyticsPage (Analytics tab) - see useDailyVisitBuckets for the bucketing
// this renders.
export function VisitChart({ daily30 }: { daily30: number[] }) {
  const [hoverIndex, setHoverIndex] = useState<number | null>(null);

  const max = Math.max(1, ...daily30);
  const hovered = hoverIndex !== null ? daily30[hoverIndex] : null;
  const hoveredDaysAgo = hoverIndex !== null ? daily30.length - 1 - hoverIndex : null;

  return (
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
  );
}

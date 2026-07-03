import { grommet, ThemeType } from "grommet";
import { deepMerge } from "grommet/utils";

// Mirrors the copper/PCB palette used in the hardware design sheet, so the
// dashboard and the schematic reference read as one system.
export const theme: ThemeType = deepMerge(grommet, {
  global: {
    colors: {
      brand: "#b96f34",
      background: { light: "#eef1ee", dark: "#0d241a" },
      "background-back": { light: "#f4f6f3", dark: "#123326" },
      "background-front": { light: "#ffffff", dark: "#163d2e" },
      "background-contrast": { light: "#00000011", dark: "#ffffff11" },
      text: { light: "#16241c", dark: "#eaf3ec" },
      "text-weak": { light: "#5b6b62", dark: "#93ab9c" },
      border: { light: "#d3d9d2", dark: "#24503c" },
      "accent-1": "#b96f34",
      "state-idle": { light: "#2f7a4f", dark: "#5fd394" },
      "state-wait": { light: "#ad7e21", dark: "#e0b458" },
      "state-cycle": { light: "#2f6fb9", dark: "#6fb3e0" },
      "state-fault": { light: "#bc4230", dark: "#e2685a" },
    },
    font: {
      family:
        "ui-sans-serif, -apple-system, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif",
    },
    edgeSize: {
      hair: "1px",
    },
  },
  card: {
    container: {
      round: "small",
      elevation: "none",
      border: { color: "border", size: "1px" },
    },
  },
});

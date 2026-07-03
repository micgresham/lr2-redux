import React from "react";
import ReactDOM from "react-dom/client";
import { Grommet } from "grommet";
import App from "./App";
import { theme } from "./theme";

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <Grommet theme={theme} themeMode="auto" full>
      <App />
    </Grommet>
  </React.StrictMode>,
);

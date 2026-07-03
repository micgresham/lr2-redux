/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_DEFAULT_DEVICE_URL?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}

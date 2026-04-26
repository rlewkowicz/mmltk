import { InjectionToken } from "@angular/core";

import type { BrowserHostTransport } from "../../host_api";
import { resolveBrowserHostTransport } from "../../transport";

export const BROWSER_HOST_TRANSPORT = new InjectionToken<BrowserHostTransport>(
  "MMLTK browser host transport",
  {
    providedIn: "root",
    factory: () => resolveBrowserHostTransport(),
  },
);

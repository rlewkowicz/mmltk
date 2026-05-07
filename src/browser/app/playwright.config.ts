import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./tests/e2e",
  testMatch: ["annotate-live-video.spec.ts"],
  fullyParallel: false,
  workers: 1,
  timeout: 120_000,
  expect: {
    timeout: 15_000,
  },
  reporter: "list",
  use: {
    screenshot: "only-on-failure",
    trace: "retain-on-failure",
  },
  projects: [
    {
      name: "cef-cdp",
    },
  ],
});

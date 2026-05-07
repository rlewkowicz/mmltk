import { chromium, type Browser, type Page } from "@playwright/test";

export interface MmltkCefConnection {
  browser: Browser;
  page: Page;
}

async function delay(ms: number): Promise<void> {
  await new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

async function waitForCdpEndpoint(
  port: number,
  timeoutMs: number,
  failureDetail: () => string = () => "",
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  const url = `http://127.0.0.1:${port}/json/version`;
  const targetsUrl = `http://127.0.0.1:${port}/json/list`;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(url);
      const targetsResponse = response.ok ? await fetch(targetsUrl) : null;
      const targets = targetsResponse?.ok ? await targetsResponse.json() : null;
      if (response.ok && hasLoadedAppTarget(targets)) {
        return;
      }
    } catch {
      // The GUI may still be starting CEF.
    }
    await delay(100);
  }
  const detail = failureDetail();
  throw new Error(
    detail.length > 0
      ? `CEF remote debugging endpoint did not open on ${url}\n\nGUI output tail:\n${detail}`
      : `CEF remote debugging endpoint did not open on ${url}`,
  );
}

function hasLoadedAppTarget(targets: unknown): boolean {
  if (!Array.isArray(targets)) {
    return false;
  }
  return targets.some((target) => {
    if (typeof target !== "object" || target === null) {
      return false;
    }
    const record = target as Record<string, unknown>;
    return (
      record["type"] === "page" &&
      typeof record["url"] === "string" &&
      record["url"].startsWith("app://mmltk.invalid/browser/") &&
      record["url"].includes("#/")
    );
  });
}

function findAppPage(browser: Browser): Page | null {
  for (const context of browser.contexts()) {
    for (const page of context.pages()) {
      if (page.url().startsWith("app://mmltk.invalid/")) {
        return page;
      }
    }
  }
  return null;
}

export async function connectToMmltkCef(
  port: number,
  timeoutMs = 60_000,
  failureDetail?: () => string,
): Promise<MmltkCefConnection> {
  await waitForCdpEndpoint(port, timeoutMs, failureDetail);
  const connectDeadline = Date.now() + timeoutMs;
  let browser: Browser | null = null;
  let lastConnectError: unknown = null;
  while (browser === null && Date.now() < connectDeadline) {
    try {
      browser = await chromium.connectOverCDP(`http://127.0.0.1:${port}`, {
        timeout: Math.min(timeoutMs, 30_000),
      });
    } catch (error) {
      lastConnectError = error;
      await delay(250);
    }
  }
  if (browser === null) {
    throw lastConnectError instanceof Error
      ? lastConnectError
      : new Error(`Failed to connect to CEF CDP on port ${port}`);
  }

  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const page = findAppPage(browser);
    if (page !== null) {
      await page.locator("app-root").waitFor({ state: "attached", timeout: 10_000 });
      await page
        .getByRole("navigation", { name: "Workflow selection" })
        .waitFor({ timeout: 20_000 });
      return { browser, page };
    }
    await delay(100);
  }

  await browser.close();
  throw new Error("CEF app page did not appear in the remote debugging target list");
}

import { describe, expect, it } from "vitest";

import bootstrapFixture from "../../src-tauri/tests/fixtures/contracts/bootstrap.json";

import type { Availability, BootstrapView } from "./contracts";

describe("frontend contracts", () => {
  it("keeps a legitimate zero distinct from unavailable data", () => {
    const measuredZero = {
      state: "available",
      value: 0,
      measuredAt: 1_700_000_000_000,
    } satisfies Availability<number>;
    const unavailable = {
      state: "notObserved",
      reason: "No sample has completed yet",
    } satisfies Availability<number>;

    expect(measuredZero).toMatchObject({ state: "available", value: 0 });
    expect(unavailable).not.toHaveProperty("value");
  });

  it("uses the camelCase bootstrap wire shape", () => {
    const bootstrap = {
      historyRevision: 4,
      liveRevision: 2,
      deviceId: "device-1",
      reportingTimeZone: {
        id: "Asia/Shanghai",
        displayName: "Asia/Shanghai",
        source: "windowsSystem",
      },
      implementedCapabilities: [
        { provider: "codex", capabilities: ["usage", "sessions"] },
      ],
      runtimeHealth: [
        { runtime: "usage", status: "healthy", updatedAt: 1_700_000_000_000 },
      ],
    } satisfies BootstrapView;

    expect(JSON.parse(JSON.stringify(bootstrap))).toEqual(bootstrap);
    expect(Object.keys(bootstrap)).toEqual([
      "historyRevision",
      "liveRevision",
      "deviceId",
      "reportingTimeZone",
      "implementedCapabilities",
      "runtimeHealth",
    ]);
  });

  it("accepts the shared Rust serialization fixture", () => {
    const expected = {
      historyRevision: 7,
      liveRevision: 0,
      deviceId: "device-synthetic-contract",
      reportingTimeZone: {
        id: "Synthetic Standard Time",
        displayName: "Synthetic Time",
        source: "windowsSystem",
      },
      implementedCapabilities: [
        {
          provider: "codex",
          capabilities: ["usage", "sessions", "context", "quota"],
        },
      ],
      runtimeHealth: [
        { runtime: "usage", status: "healthy", updatedAt: 1_786_147_200_000 },
        {
          runtime: "live",
          status: "notStarted",
          updatedAt: 1_786_147_200_000,
        },
        {
          runtime: "limits",
          status: "notStarted",
          updatedAt: 1_786_147_200_000,
        },
      ],
    } satisfies BootstrapView;

    expect(bootstrapFixture).toEqual(expected);
  });
});

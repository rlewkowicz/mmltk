/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineLazyGetter(
  lazy,
  "JsonSchemaValidator",
  () =>
    ChromeUtils.importESModule(
      "resource://gre/modules/components-utils/JsonSchemaValidator.sys.mjs"
    ).JsonSchemaValidator
);
ChromeUtils.defineLazyGetter(lazy, "logConsole", () =>
  console.createInstance({
    prefix: "GuardianTypes",
    maxLogLevel: Services.prefs.getBoolPref("browser.ipProtection.log", false)
      ? "Debug"
      : "Warn",
  })
);

export class ProxyPass extends EventTarget {
  #body = {
    nbf: 0,
    exp: 0,
  };
  constructor(token) {
    super();
    if (typeof token !== "string") {
      throw new TypeError(
        "Invalid arguments for ProxyPass constructor, token is not a string"
      );
    }
    this.token = token;
    const parts = this.token.split(".");
    if (parts.length !== 3) {
      throw new TypeError("Invalid token format");
    }
    try {
      const body = JSON.parse(atob(parts[1]));
      if (
        !lazy.JsonSchemaValidator.validate(body, ProxyPass.bodySchema).valid
      ) {
        throw new TypeError("Token body does not match schema");
      }
      this.#body = body;
    } catch (error) {
      throw new TypeError("Invalid token format: " + error.message);
    }
  }

  isValid(now = Temporal.Now.instant()) {
    return (
      Temporal.Instant.compare(now, this.from) >= 0 &&
      Temporal.Instant.compare(now, this.until) < 0
    );
  }

  shouldRotate(now = Temporal.Now.instant()) {
    if (!this.isValid(now)) {
      return true;
    }
    return Temporal.Instant.compare(now, this.rotationTimePoint) >= 0;
  }

  get from() {
    return Temporal.Instant.fromEpochMilliseconds(this.#body.nbf * 1000);
  }

  get until() {
    return Temporal.Instant.fromEpochMilliseconds(this.#body.exp * 1000);
  }

  static async fromResponse(response) {
    if (!response.ok) {
      lazy.logConsole.error(
        `Failed to fetch proxy pass: ${response.status} ${response.statusText}`
      );
      return null;
    }

    try {
      const responseData = await response.json();
      const token = responseData?.token;

      if (!token || typeof token !== "string") {
        lazy.logConsole.error("Missing or invalid token in response");
        return null;
      }
      return new ProxyPass(token);
    } catch (error) {
      lazy.logConsole.error("Error parsing proxy pass response:", error);
      return null;
    }
  }

  get rotationTimePoint() {
    return this.until.subtract(ProxyPass.ROTATION_TIME);
  }

  asBearerToken() {
    return `Bearer ${this.token}`;
  }

  static ROTATION_TIME = Temporal.Duration.from({ minutes: 2 });

  static get bodySchema() {
    return {
      $schema: "http://json-schema.org/draft-07/schema#",
      title: "JWT Claims",
      type: "object",
      properties: {
        sub: {
          type: "string",
          description: "Subject identifier",
        },
        aud: {
          type: "string",
          format: "uri",
          description: "Audience for which the token is intended",
        },
        iat: {
          type: "integer",
          description: "Issued-at time (seconds since Unix epoch)",
        },
        nbf: {
          type: "integer",
          description: "Not-before time (seconds since Unix epoch)",
        },
        exp: {
          type: "integer",
          description: "Expiration time (seconds since Unix epoch)",
        },
        iss: {
          type: "string",
          description: "Issuer identifier",
        },
      },
      required: ["sub", "aud", "iat", "nbf", "exp", "iss"],
      additionalProperties: true,
    };
  }
}

export class Entitlement {
  subscribed = false;
  uid = 0;
  maxBytes = BigInt(0);
  limitedBandwidth = true;

  constructor(
    args = {
      subscribed: false,
      uid: 0,
      maxBytes: "0",
      limited_bandwidth: true,
    }
  ) {
    this.subscribed = args.subscribed;
    this.uid = args.uid;
    this.maxBytes = BigInt(args.maxBytes);
    this.limitedBandwidth = args.limited_bandwidth ?? true;
    Object.freeze(this);
  }

  static fromResponse(response) {
    if (!response.ok) {
      return null;
    }
    return response.json().then(data => {
      const result = lazy.JsonSchemaValidator.validate(
        data,
        Entitlement.schema
      );
      if (!result.valid) {
        return null;
      }
      return new Entitlement(data);
    });
  }

  static get schema() {
    return {
      $schema: "http://json-schema.org/draft-07/schema#",
      title: "Entitlement",
      type: "object",
      properties: {
        subscribed: {
          type: "boolean",
        },
        uid: {
          type: "integer",
        },
        maxBytes: {
          type: "string",
          description:
            "A BigInt string representing the maximum number of bytes allowed for the user",
        },
        limited_bandwidth: {
          type: "boolean",
          description:
            "True if the user has a bandwidth cap; false if the user has unlimited bandwidth",
        },
      },
      required: ["subscribed", "uid", "maxBytes"],
      additionalProperties: true,
    };
  }

  toString() {
    return JSON.stringify({
      subscribed: this.subscribed,
      uid: this.uid,
      maxBytes: this.maxBytes.toString(),
      limited_bandwidth: this.limitedBandwidth,
    });
  }
}

export class ProxyUsage {
  max = null;
  remaining = null;
  reset = null;
  unlimited = false;

  constructor(max, remaining, reset, unlimited) {
    this.unlimited = !!unlimited;

    if (this.unlimited) {
      Object.freeze(this);
      return;
    }

    if (max === null || remaining === null || reset === null) {
      throw new TypeError("invalid usage");
    }

    this.max = BigInt(max);
    if (this.max < BigInt(0)) {
      throw new TypeError("max must be non-negative");
    }

    this.remaining = BigInt(remaining);
    if (this.remaining < BigInt(0)) {
      throw new TypeError("remaining must be non-negative");
    }

    if (this.remaining > this.max) {
      throw new TypeError("remaining cannot exceed max");
    }

    this.reset = Temporal.Instant.from(reset);

    Object.freeze(this);
  }

  equals(other) {
    if (!(other instanceof ProxyUsage)) {
      return false;
    }
    if (
      this.unlimited !== other.unlimited ||
      this.max !== other.max ||
      this.remaining !== other.remaining
    ) {
      return false;
    }
    if (this.reset === null || other.reset === null) {
      return this.reset === other.reset;
    }
    return Temporal.Instant.compare(this.reset, other.reset) === 0;
  }

  static fromResponse(response) {
    const unlimited = response.headers.get("X-Quota-Unlimited") === "true";

    if (unlimited) {
      return new ProxyUsage(null, null, null, true);
    }

    const getOrThrow = headerName => {
      const value = response.headers.get(headerName);
      if (!value) {
        throw new TypeError(`Missing required header: ${headerName}`);
      }
      return value;
    };

    const quotaLimit = getOrThrow("X-Quota-Limit");
    const quotaRemaining = getOrThrow("X-Quota-Remaining");
    const quotaReset = getOrThrow("X-Quota-Reset");

    return new ProxyUsage(quotaLimit, quotaRemaining, quotaReset);
  }

  get quotaExhausted() {
    return !this.unlimited && this.remaining <= 0;
  }
}

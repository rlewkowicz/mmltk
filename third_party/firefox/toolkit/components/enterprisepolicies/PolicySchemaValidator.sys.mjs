/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "JsonSchema", () => {
  return ChromeUtils.importESModule("resource://gre/modules/JsonSchema.sys.mjs")
    .JsonSchema;
});

ChromeUtils.defineLazyGetter(lazy, "log", () => {
  let { ConsoleAPI } = ChromeUtils.importESModule(
    "resource://gre/modules/Console.sys.mjs"
  );
  return new ConsoleAPI({
    prefix: "PolicySchemaValidator",
    maxLogLevel: "error",
  });
});

const JSON_MEDIA_TYPE = "application/json";

export function validate(
  value,
  schema,
  { allowAdditionalProperties = false } = {}
) {
  try {
    const normalized = trimInvalidListItems(normalize(value, schema), schema);

    const { valid, errors } = lazy.JsonSchema.validate(normalized, schema);
    if (!valid) {
      return { valid: false, error: new Error(formatErrors(errors)) };
    }

    return {
      valid: true,
      parsedValue: hydrate(normalized, schema, allowAdditionalProperties),
    };
  } catch (ex) {
    return { valid: false, error: ex };
  }
}

class PolicyParameterError extends Error {
  constructor(message) {
    super(message);
    this.name = "PolicyParameterError";
  }
}

function formatErrors(errors) {
  return errors.map(e => `${e.error} (at ${e.instanceLocation})`).join("; ");
}

function valueToString(value) {
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function effectiveTypes(schema) {
  let types = [];
  if (Array.isArray(schema.type)) {
    types.push(...schema.type);
  } else if (schema.type !== undefined) {
    types.push(schema.type);
  }
  if (Array.isArray(schema.anyOf)) {
    for (let branch of schema.anyOf) {
      if (Array.isArray(branch.type)) {
        types.push(...branch.type);
      } else if (branch.type !== undefined) {
        types.push(branch.type);
      }
    }
  }
  return types;
}

function subschemaForProperty(schema, key) {
  if (schema.properties && Object.hasOwn(schema.properties, key)) {
    return schema.properties[key];
  }
  if (schema.patternProperties) {
    for (const pattern of Object.keys(schema.patternProperties)) {
      if (new RegExp(pattern).test(key)) {
        return schema.patternProperties[pattern];
      }
    }
  }
  return undefined;
}

function isUriSchema(schema) {
  if (schema.format === "uri") {
    return true;
  }
  if (Array.isArray(schema.anyOf)) {
    return schema.anyOf.some(branch => branch.format === "uri");
  }
  return false;
}

function normalize(value, schema) {
  if (!schema || typeof schema != "object") {
    return value;
  }

  if (schema.contentMediaType === JSON_MEDIA_TYPE && typeof value == "string") {
    try {
      value = JSON.parse(value);
    } catch {
      throw new PolicyParameterError(
        `value is not valid JSON: ${valueToString(value)}`
      );
    }
  }

  const types = effectiveTypes(schema);

  if (
    types.includes("boolean") &&
    !types.includes("number") &&
    !types.includes("integer") &&
    typeof value == "number" &&
    (value === 0 || value === 1)
  ) {
    return !!value;
  }

  if (value && typeof value == "object" && !Array.isArray(value)) {
    if (schema.properties || schema.patternProperties) {
      const result = {};
      for (const key of Object.keys(value)) {
        const subschema = subschemaForProperty(schema, key);
        result[key] = subschema ? normalize(value[key], subschema) : value[key];
      }
      return result;
    }
    return value;
  }

  if (Array.isArray(value) && schema.items) {
    return value.map(item => normalize(item, schema.items));
  }

  return value;
}

function trimInvalidListItems(value, schema) {
  if (!schema || typeof schema != "object" || value == null) {
    return value;
  }

  if (typeof value == "object" && !Array.isArray(value)) {
    if (schema.properties || schema.patternProperties) {
      const result = {};
      for (const key of Object.keys(value)) {
        const subschema = subschemaForProperty(schema, key);
        result[key] = subschema
          ? trimInvalidListItems(value[key], subschema)
          : value[key];
      }
      return result;
    }
    return value;
  }

  if (Array.isArray(value) && schema.items) {
    const result = [];
    for (const item of value) {
      const trimmedItem = trimInvalidListItems(item, schema.items);
      if (lazy.JsonSchema.validate(trimmedItem, schema.items).valid) {
        result.push(trimmedItem);
      } else {
        lazy.log.error(`Ignoring invalid list entry ${valueToString(item)}.`);
      }
    }
    return result;
  }

  return value;
}

function hydrate(value, schema, allowAdditionalProperties) {
  if (!schema || typeof schema != "object" || value == null) {
    return value;
  }

  if (isUriSchema(schema) && typeof value == "string") {
    if (value === "") {
      return "";
    }
    try {
      return new URL(value);
    } catch {
      return value;
    }
  }

  if (typeof value == "object" && !Array.isArray(value)) {
    if (schema.properties || schema.patternProperties) {
      const result = {};
      for (const key of Object.keys(value)) {
        const subschema = subschemaForProperty(schema, key);
        if (!subschema) {
          if (allowAdditionalProperties) {
            continue;
          }
          throw new PolicyParameterError(
            `Object has unexpected property '${key}'`
          );
        }
        result[key] = hydrate(value[key], subschema, allowAdditionalProperties);
      }
      return result;
    }
    return value;
  }

  if (Array.isArray(value) && schema.items) {
    return value.map(item =>
      hydrate(item, schema.items, allowAdditionalProperties)
    );
  }

  return value;
}

export const PolicySchemaValidator = { validate };

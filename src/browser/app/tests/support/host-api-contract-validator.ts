import type { IntentMessage, Workflow } from "../../src/host_api";
import { HOST_API_INTENTS, HOST_API_WORKFLOWS } from "../../src/host_api.generated";
import {
  HOST_API_INTENT_WORKFLOWS,
  HOST_API_SCHEMA_BUNDLE,
  hostApiIntentPayloadSchema,
  type HostApiJsonSchema,
} from "../../src/workflow_contract.generated";

type JsonRecord = Record<string, unknown>;

function isRecord(value: unknown): value is JsonRecord {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function schemaRecord(value: unknown): HostApiJsonSchema {
  return isRecord(value) ? (value as HostApiJsonSchema) : {};
}

function schemaArray(value: unknown): readonly unknown[] {
  return Array.isArray(value) ? value : [];
}

function localRefSchema(ref: string): HostApiJsonSchema {
  if (!ref.startsWith("#/components/schemas/")) {
    throw new Error(`unsupported host API schema ref ${ref}`);
  }
  const schemaName = ref.slice("#/components/schemas/".length);
  const schemas = HOST_API_SCHEMA_BUNDLE.schemas as Readonly<Record<string, HostApiJsonSchema>>;
  const schema = schemas[schemaName];
  if (schema === undefined) {
    throw new Error(`unresolved host API schema ref ${ref}`);
  }
  return schema;
}

function resolveSchema(schema: HostApiJsonSchema): HostApiJsonSchema {
  const ref = schema.$ref;
  return typeof ref === "string" ? resolveSchema(localRefSchema(ref)) : schema;
}

function schemaType(schema: HostApiJsonSchema): string | undefined {
  if (typeof schema.type === "string") {
    return schema.type;
  }
  if (Array.isArray(schema.type)) {
    return schema.type.find((entry): entry is string => typeof entry === "string" && entry !== "null");
  }
  if (isRecord(schema.properties)) {
    return "object";
  }
  if (isRecord(schema.items)) {
    return "array";
  }
  return undefined;
}

function valueMatchesType(value: unknown, type: string): boolean {
  switch (type) {
    case "object":
      return isRecord(value);
    case "array":
      return Array.isArray(value);
    case "string":
      return typeof value === "string";
    case "number":
    case "integer":
      return typeof value === "number" && Number.isFinite(value);
    case "boolean":
      return typeof value === "boolean";
    case "null":
      return value === null;
    default:
      return true;
  }
}

function validateSchemaValue(
  schemaInput: HostApiJsonSchema,
  value: unknown,
  path: string,
  errors: string[],
): void {
  const schema = resolveSchema(schemaInput);
  const oneOf = schemaArray(schema.oneOf);
  if (oneOf.length > 0) {
    const anyValid = oneOf.some((candidate) => {
      const candidateErrors: string[] = [];
      validateSchemaValue(schemaRecord(candidate), value, path, candidateErrors);
      return candidateErrors.length === 0;
    });
    if (!anyValid) {
      errors.push(`${path} must match one allowed schema`);
    }
    return;
  }

  const type = schemaType(schema);
  if (type !== undefined && !valueMatchesType(value, type)) {
    errors.push(`${path} must be ${type}`);
    return;
  }

  const enumValues = schemaArray(schema.enum);
  if (enumValues.length > 0 && !enumValues.some((entry) => Object.is(entry, value))) {
    errors.push(`${path} must be one of ${enumValues.map(String).join(", ")}`);
    return;
  }

  if (type === "array") {
    const items = schemaRecord(schema.items);
    (value as unknown[]).forEach((entry, index) => {
      validateSchemaValue(items, entry, `${path}[${index}]`, errors);
    });
    return;
  }

  if (type !== "object") {
    return;
  }

  const record = value as JsonRecord;
  const properties = isRecord(schema.properties) ? schema.properties : {};
  for (const required of schemaArray(schema.required)) {
    if (typeof required === "string" && !(required in record)) {
      errors.push(`${path}.${required} is required`);
    }
  }

  for (const [key, childValue] of Object.entries(record)) {
    const childSchema = properties[key];
    if (childSchema !== undefined) {
      validateSchemaValue(schemaRecord(childSchema), childValue, `${path}.${key}`, errors);
      continue;
    }
    if (schema.additionalProperties === false) {
      errors.push(`${path}.${key} is not allowed`);
      continue;
    }
    if (isRecord(schema.additionalProperties)) {
      validateSchemaValue(schemaRecord(schema.additionalProperties), childValue, `${path}.${key}`, errors);
    }
  }
}

export function validateHostApiSchema(
  schema: HostApiJsonSchema,
  value: unknown,
  path = "payload",
): string[] {
  const errors: string[] = [];
  validateSchemaValue(schema, value, path, errors);
  return errors;
}

export function validateHostApiSettingsPatch(patch: unknown): string[] {
  return validateHostApiSchema(
    { $ref: "#/components/schemas/SettingsPatch" },
    patch,
    "payload.patch",
  );
}

export function validateHostApiIntent(intent: IntentMessage): string[] {
  const errors: string[] = [];
  if (!HOST_API_WORKFLOWS.includes(intent.workflow as Workflow)) {
    errors.push(`workflow ${intent.workflow} is not generated by the host API contract`);
  }
  if (!HOST_API_INTENTS.includes(intent.intent as (typeof HOST_API_INTENTS)[number])) {
    errors.push(`intent ${intent.intent} is not generated by the host API contract`);
  }

  const workflowMap = HOST_API_INTENT_WORKFLOWS as Readonly<Record<string, readonly Workflow[]>>;
  const allowedWorkflows = workflowMap[intent.intent];
  if (allowedWorkflows !== undefined && !allowedWorkflows.includes(intent.workflow)) {
    errors.push(`intent ${intent.intent} is not allowed for workflow ${intent.workflow}`);
  }

  const schema = hostApiIntentPayloadSchema(intent.intent);
  if (schema === undefined) {
    errors.push(`intent ${intent.intent} is missing a generated payload schema`);
    return errors;
  }
  errors.push(...validateHostApiSchema(schema, intent.payload));
  return errors;
}

export function assertHostApiIntent(intent: IntentMessage): void {
  const errors = validateHostApiIntent(intent);
  if (errors.length > 0) {
    throw new Error(`invalid host API intent ${intent.intent}: ${errors.join("; ")}`);
  }
}

export function assertHostApiSettingsPatch(patch: unknown): void {
  const errors = validateHostApiSettingsPatch(patch);
  if (errors.length > 0) {
    throw new Error(`invalid host API settings patch: ${errors.join("; ")}`);
  }
}

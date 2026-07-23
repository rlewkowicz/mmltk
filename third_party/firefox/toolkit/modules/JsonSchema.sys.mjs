/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


const sandbox = new Cu.Sandbox(null, {
  wantComponents: false,
  wantGlobalProperties: ["URL"],
});

Services.scriptloader.loadSubScript(
  "chrome://global/content/third_party/cfworker/json-schema.js",
  sandbox
);

Cu.exportFunction(
  function validateMozUrlFormat(input) {
    try {
      const formatted = Services.urlFormatter.formatURL(input);
      return Cu.waiveXrays(sandbox.fastFormat).uri(formatted);
    } catch {
      return false;
    }
  },
  sandbox.fastFormat,
  { defineAs: "moz-url-format" }
);

Cu.evalInSandbox(
  `this.initialBaseURI = initialBaseURI = new URL("http://mozilla.org");`,
  sandbox
);

class Validator {
  #inner;
  #draft;

  constructor(
    schema,
    { draft = detectSchemaDraft(schema), shortCircuit = true } = {}
  ) {
    this.#draft = draft;
    this.#inner = Cu.waiveXrays(
      new sandbox.Validator(Cu.cloneInto(schema, sandbox), draft, shortCircuit)
    );
  }

  validate(instance) {
    try {
      return this.#inner.validate(Cu.cloneInto(instance, sandbox));
    } catch (ex) {
      throw new Error(ex.message, { cause: ex });
    }
  }

  addSchema(schema, id) {
    const draft = detectSchemaDraft(schema, undefined);
    if (draft && this.#draft != draft) {
      console.error(
        `Adding a draft "${draft}" schema to a draft "${
          this.#draft
        }" validator.`
      );
    }
    try {
      this.#inner.addSchema(Cu.cloneInto(schema, sandbox), id);
    } catch (ex) {
      throw new Error(ex.message, { cause: ex });
    }
  }
}

function validate(
  instance,
  schema,
  { draft = detectSchemaDraft(schema), shortCircuit = true } = {}
) {
  const clonedSchema = Cu.cloneInto(schema, sandbox);

  try {
    return sandbox.validate(
      Cu.cloneInto(instance, sandbox),
      clonedSchema,
      draft,
      sandbox.dereference(clonedSchema),
      shortCircuit
    );
  } catch (ex) {
    throw new Error(ex.message, { cause: ex });
  }
}

function detectSchemaDraft(schema, defaultDraft = "2019-09") {
  const { $schema } = schema;

  if (typeof $schema === "undefined") {
    return defaultDraft;
  }

  switch ($schema) {
    case "http://json-schema.org/draft-04/schema#":
      return "4";

    case "http://json-schema.org/draft-06/schema#":
      return "6";

    case "http://json-schema.org/draft-07/schema#":
      return "7";

    case "https://json-schema.org/draft/2019-09/schema":
      return "2019-09";

    case "https://json-schema.org/draft/2020-12/schema":
      return "2020-12";

    default:
      console.error(
        `Unexpected $schema "${$schema}", defaulting to ${defaultDraft}.`
      );
      return defaultDraft;
  }
}

export const JsonSchema = {
  Validator,
  validate,
  detectSchemaDraft,
};

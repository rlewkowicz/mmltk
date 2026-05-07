#!/usr/bin/env node
"use strict";

const fs = require("node:fs");
const path = require("node:path");

const appRoot = path.resolve(__dirname, "..");
const requiredArtifacts = [
  path.join(appRoot, "src", "host_api.generated.ts"),
  path.join(appRoot, "src", "workflow_contract.generated.ts"),
];

for (const artifact of requiredArtifacts) {
  if (!fs.existsSync(artifact)) {
    console.error(`Missing generated native browser contract artifact: ${path.relative(appRoot, artifact)}`);
    process.exitCode = 1;
  }
}

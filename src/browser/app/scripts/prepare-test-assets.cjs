"use strict";

const fs = require("node:fs");
const path = require("node:path");

const projectRoot = process.cwd();
const testDist = path.join(projectRoot, ".test-dist");
const shaderSourceDir = path.join(projectRoot, "src", "shaders");
const shaderOutputDir = path.join(testDist, "src", "shaders");

fs.mkdirSync(shaderOutputDir, { recursive: true });

for (const entry of fs.readdirSync(shaderSourceDir, { withFileTypes: true })) {
  if (!entry.isFile() || !entry.name.endsWith(".wgsl")) {
    continue;
  }
  const shaderSource = fs.readFileSync(path.join(shaderSourceDir, entry.name), "utf8");
  const compiledModule = `"use strict";\nmodule.exports = ${JSON.stringify(shaderSource)};\n`;
  fs.writeFileSync(path.join(shaderOutputDir, entry.name), compiledModule);
}

fs.writeFileSync(
  path.join(testDist, "package.json"),
  `${JSON.stringify({ type: "commonjs" })}\n`,
);

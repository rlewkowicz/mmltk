import { nodeResolve } from "@rollup/plugin-node-resolve";
import typescript from "@rollup/plugin-typescript";
import copy from "rollup-plugin-copy";

function wgslPlugin() {
  return {
    name: "wgsl-plugin",
    transform(code, id) {
      if (!id.endsWith(".wgsl")) {
        return null;
      }

      return {
        code: `export default ${JSON.stringify(code)};`,
        map: { mappings: "" },
      };
    },
  };
}

export default {
  input: "src/main.ts",
  output: {
    file: "dist/app.js",
    format: "esm",
    sourcemap: true,
  },
  plugins: [
    wgslPlugin(),
    nodeResolve(),
    typescript({ tsconfig: "./tsconfig.json" }),
    copy({
      targets: [
        { src: "index.html", dest: "dist" },
      ],
      hook: "writeBundle",
    }),
  ],
  watch: {
    clearScreen: false,
  },
};

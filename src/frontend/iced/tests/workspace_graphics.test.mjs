import test from "node:test";
import assert from "node:assert/strict";
import { bindWorkspace } from "../src/presentation_surface/graphics.mjs";

function environment() {
    let configured = [];
    let queries = 0;
    const listeners = new Map();
    let mutation;
    let disconnected = false;
    globalThis.document = {
        querySelectorAll() {
            queries++;
            return configured.map(device => ({ getContext() { return { getConfiguration() { return { device }; } }; } }));
        },
        addEventListener(name, callback) { listeners.set(name, callback); },
        removeEventListener(name, callback) { assert.equal(listeners.get(name), callback); listeners.delete(name); },
    };
    globalThis.MutationObserver = class {
        constructor(callback) { mutation = callback; }
        observe() {}
        disconnect() { disconnected = true; }
    };
    const configure = (...devices) => {
        configured = devices;
        listeners.get("mmltk-workspace-configuration")?.({ isTrusted: true });
    };
    return {
        configure,
        added() { mutation([{ addedNodes: [{ nodeName: "CANVAS" }], removedNodes: [] }]); },
        queries() { return queries; },
        disposed() { assert.equal(listeners.size, 0); assert.equal(disconnected, true); },
        cleanup() { delete globalThis.document; delete globalThis.MutationObserver; },
    };
}

function device() {
    let lose;
    const result = {
        binds: 0, unbinds: 0,
        bindWorkspace() { this.binds++; },
        unbindWorkspace() { this.unbinds++; },
        lost: new Promise(resolve => { lose = resolve; }),
        lose() { lose(); },
    };
    return result;
}

const flush = async () => { await Promise.resolve(); await Promise.resolve(); };

test("live configuration replaces devices, ignores stale loss, and disposes exactly once", async () => {
    const env = environment();
    const a = device(), b = device();
    const stop = bindWorkspace();
    try {
        await flush();
        env.configure(a);
        await flush();
        assert.equal(a.binds, 1);
        env.configure(b); // No loss notification from A is needed.
        await flush();
        assert.equal(a.unbinds, 1);
        assert.equal(b.binds, 1);
        a.lose();
        await flush();
        assert.equal(b.unbinds, 0);
        env.configure(b);
        await flush();
        assert.equal(b.binds, 1);
        stop(); stop();
        assert.equal(a.unbinds, 1);
        assert.equal(b.unbinds, 1);
        env.disposed();
    } finally { stop(); env.cleanup(); }
});

test("ambiguity and unconfigured canvases wait for events without polling", async () => {
    const env = environment();
    const a = device(), b = device();
    const stop = bindWorkspace();
    try {
        env.configure(a, b);
        await flush();
        const queries = env.queries();
        await flush();
        assert.equal(env.queries(), queries);
        assert.equal(a.binds + b.binds, 0);
        env.configure(a);
        env.added();
        await flush();
        assert.equal(a.binds, 1);
        a.lose();
        await flush();
        assert.equal(a.unbinds, 1);
        env.configure(b);
        await flush();
        assert.equal(b.binds, 1);
    } finally { stop(); env.cleanup(); }
});

test("stop during initial discovery cancels binding and removes all observers", async () => {
    const env = environment();
    const a = device();
    const stop = bindWorkspace();
    env.configure(a);
    stop();
    try {
        await flush();
        assert.equal(a.binds, 0);
        assert.equal(env.queries(), 0);
        env.disposed();
    } finally { env.cleanup(); }
});

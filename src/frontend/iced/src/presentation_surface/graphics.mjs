// Configuration events come from the canvas owner after its actual WebGPU
// configure/unconfigure operation. No application record authorizes a binding.
export function bindWorkspace() {
    let stopped = false;
    let queued = false;
    let device = null;
    const retired = new WeakSet();
    const replace = next => {
        if (device === next) return;
        const previous = device;
        device = next;
        previous?.unbindWorkspace();
        if (!next) return;
        next.bindWorkspace();
        next.lost.then(() => {
            retired.add(next);
            if (!stopped && device === next) {
                replace(null);
                schedule();
            }
        });
    };
    const discover = () => {
        queued = false;
        if (stopped) return;
        const devices = new Set();
        for (const canvas of document.querySelectorAll("canvas")) {
            let configured;
            try { configured = canvas.getContext("webgpu")?.getConfiguration?.()?.device; }
            catch { continue; }
            if (configured && !retired.has(configured)) devices.add(configured);
        }
        replace(devices.size === 1 ? devices.values().next().value : null);
    };
    const schedule = () => {
        if (!stopped && !queued) {
            queued = true;
            queueMicrotask(discover);
        }
    };
    const configuration = event => { if (event.isTrusted) schedule(); };
    document.addEventListener("mmltk-workspace-configuration", configuration, true);
    const observer = new MutationObserver(records => {
        if (records.some(record => [...record.addedNodes, ...record.removedNodes].some(node =>
            node.nodeName === "CANVAS" || node.querySelector?.("canvas")))) schedule();
    });
    observer.observe(document, { childList: true, subtree: true });
    schedule();
    return () => {
        if (stopped) return;
        stopped = true;
        observer.disconnect();
        document.removeEventListener("mmltk-workspace-configuration", configuration, true);
        replace(null);
    };
}

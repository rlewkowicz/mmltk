export {};

declare global {
  type DeferredTask =
    import("resource://gre/modules/DeferredTask.sys.mjs").DeferredTask;
}

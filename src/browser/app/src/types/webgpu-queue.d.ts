export {};

declare global {
  interface GPUQueue {
    onSubmittedWorkDone(): Promise<undefined>;
  }
}

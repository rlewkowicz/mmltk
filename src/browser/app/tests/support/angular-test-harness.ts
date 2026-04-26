import {
  DestroyRef,
  Injector,
  runInInjectionContext,
  type StaticProvider,
  ɵChangeDetectionScheduler as ChangeDetectionSchedulerToken,
  ɵEffectScheduler as EffectSchedulerToken,
} from "@angular/core";

import { BROWSER_HOST_TRANSPORT } from "../../src/app/state/browser-shell.tokens";
import type {
  BrowserHostBridgeState,
  BrowserHostTransport,
  IntentMessage,
  StateSnapshot,
} from "../../src/host_api";

export interface FlushableEffectScheduler {
  flush(): void;
}

export class SpyTransport implements BrowserHostTransport {
  readonly dispatched: IntentMessage[] = [];
  private readonly listeners = new Set<(snapshot: StateSnapshot) => void>();
  private readonly bridgeListeners =
    new Set<(state: BrowserHostBridgeState) => void>();
  private bridgeState: BrowserHostBridgeState;

  constructor(
    private snapshot: StateSnapshot,
    readonly mode: BrowserHostTransport["mode"] = "mock",
    private readonly scheduler?: FlushableEffectScheduler,
  ) {
    this.bridgeState = {
      phase: "idle",
      connected: true,
      lastError: "",
      lastSuccessRevision: snapshot.state_revision,
    };
  }

  getSnapshot(): StateSnapshot {
    return this.snapshot;
  }

  getBridgeState(): BrowserHostBridgeState {
    return this.bridgeState;
  }

  dispatch(intent: IntentMessage): void {
    this.dispatched.push(intent);
  }

  subscribe(listener: (snapshot: StateSnapshot) => void): () => void {
    this.listeners.add(listener);
    listener(this.snapshot);
    return () => {
      this.listeners.delete(listener);
    };
  }

  subscribeBridgeState(
    listener: (state: BrowserHostBridgeState) => void,
  ): () => void {
    this.bridgeListeners.add(listener);
    listener(this.bridgeState);
    return () => {
      this.bridgeListeners.delete(listener);
    };
  }

  publish(snapshot: StateSnapshot): void {
    this.snapshot = snapshot;
    for (const listener of this.listeners) {
      listener(snapshot);
    }
    this.scheduler?.flush();
  }

  publishBridgeState(state: BrowserHostBridgeState): void {
    this.bridgeState = state;
    for (const listener of this.bridgeListeners) {
      listener(state);
    }
    this.scheduler?.flush();
  }
}

export class TestDestroyRef extends DestroyRef {
  private readonly callbacks = new Set<() => void>();
  private isDestroyed = false;

  override onDestroy(callback: () => void): () => void {
    this.callbacks.add(callback);
    return () => {
      this.callbacks.delete(callback);
    };
  }

  override get destroyed(): boolean {
    return this.isDestroyed;
  }

  destroy(): void {
    if (this.isDestroyed) {
      return;
    }
    this.isDestroyed = true;
    for (const callback of this.callbacks) {
      callback();
    }
    this.callbacks.clear();
  }
}

export class TestChangeDetectionScheduler {
  readonly runningTick = false;

  notify(): void {}
}

export interface TestSchedulableEffect {
  readonly dirty: boolean;
  run(): void;
}

export class ImmediateEffectScheduler implements FlushableEffectScheduler {
  private readonly queued = new Set<TestSchedulableEffect>();
  private dirtyEffectCount = 0;

  add(handle: TestSchedulableEffect): void {
    this.queued.add(handle);
    this.schedule(handle);
  }

  schedule(handle: TestSchedulableEffect): void {
    this.queued.add(handle);
    if (!handle.dirty) {
      return;
    }
    this.dirtyEffectCount += 1;
  }

  remove(handle: TestSchedulableEffect): void {
    if (!this.queued.delete(handle) || !handle.dirty) {
      return;
    }
    this.dirtyEffectCount = Math.max(0, this.dirtyEffectCount - 1);
  }

  flush(): void {
    while (this.dirtyEffectCount > 0) {
      let ranEffect = false;
      for (const handle of this.queued) {
        if (!handle.dirty) {
          continue;
        }
        this.dirtyEffectCount -= 1;
        ranEffect = true;
        handle.run();
      }
      if (!ranEffect) {
        this.dirtyEffectCount = 0;
      }
    }
  }
}

export type AngularServiceHarness = {
  injector: Injector;
  transport: SpyTransport;
  destroy: () => void;
  flush: () => void;
  run<T>(callback: () => T): T;
};

export function createAngularServiceHarness(
  snapshot: StateSnapshot,
  providers: StaticProvider[],
  options: {
    mode?: BrowserHostTransport["mode"];
  } = {},
): AngularServiceHarness {
  const scheduler = new ImmediateEffectScheduler();
  const transport = new SpyTransport(
    snapshot,
    options.mode ?? "mock",
    scheduler,
  );
  const destroyRef = new TestDestroyRef();
  const injector = Injector.create({
    providers: [
      { provide: DestroyRef, useValue: destroyRef },
      { provide: BROWSER_HOST_TRANSPORT, useValue: transport },
      {
        provide: ChangeDetectionSchedulerToken,
        useValue: new TestChangeDetectionScheduler(),
      },
      {
        provide: EffectSchedulerToken,
        useValue: scheduler,
      },
      ...providers,
    ],
  });

  return {
    injector,
    transport,
    destroy: () => {
      destroyRef.destroy();
    },
    flush: () => {
      scheduler.flush();
    },
    run<T>(callback: () => T): T {
      return runInInjectionContext(injector, callback);
    },
  };
}

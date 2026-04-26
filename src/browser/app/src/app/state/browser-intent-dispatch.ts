import { clampNumber } from "../../app_shared";
import type {
  BrowserIntentActionViewState,
  BrowserIntentNumericViewState,
  BrowserIntentToggleViewState,
} from "../../browser_shell_state";
import type { IntentMessage, Workflow } from "../../host_api";

export type BrowserIntentDispatcher = (
  workflow: Workflow,
  intent: string,
  payload: Record<string, unknown>,
) => IntentMessage;

export function dispatchIntentAction(
  dispatch: BrowserIntentDispatcher,
  control: BrowserIntentActionViewState,
  payload: Record<string, unknown> = {},
): IntentMessage | null {
  if (!control.enabled || control.intent === null) {
    return null;
  }
  return dispatch(control.workflow, control.intent, payload);
}

export function dispatchIntentToggle(
  dispatch: BrowserIntentDispatcher,
  control: BrowserIntentToggleViewState,
  value: boolean,
): IntentMessage | null {
  if (control.payloadKey === null) {
    return dispatchIntentAction(dispatch, control);
  }
  return dispatchIntentAction(dispatch, control, {
    [control.payloadKey]: value,
  });
}

export function dispatchIntentNumeric(
  dispatch: BrowserIntentDispatcher,
  control: BrowserIntentNumericViewState,
  value: number,
): IntentMessage | null {
  const clamped = clampNumber(Math.round(value), control.min, control.max);
  if (control.payloadKey === null) {
    return dispatchIntentAction(dispatch, control);
  }
  return dispatchIntentAction(dispatch, control, {
    [control.payloadKey]: clamped,
  });
}

export function dispatchParsedIntentNumeric(
  dispatch: BrowserIntentDispatcher,
  control: BrowserIntentNumericViewState,
  value: string | number,
): IntentMessage | null {
  const parsed = typeof value === "number" ? value : Number(value);
  return Number.isFinite(parsed)
    ? dispatchIntentNumeric(dispatch, control, parsed)
    : null;
}

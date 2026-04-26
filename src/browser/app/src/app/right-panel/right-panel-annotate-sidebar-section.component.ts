import {
  ChangeDetectionStrategy,
  Component,
  effect,
  inject,
  signal,
} from "@angular/core";
import { FormsModule } from "@angular/forms";

import { defaultAnnotateColorRangeState } from "../../app_shared";
import type {
  AnnotateColorRangeState,
  AnnotateColorToleranceState,
  AnnotateSidebarSelectedObjectState,
} from "../../host_api";
import { BrowserAnnotateSidebarState } from "../state/browser-annotate-sidebar.service";

type AnnotateMaskRangeKey = "sup" | "nosup";
type AnnotateMaskCenterKey = keyof AnnotateColorRangeState["center"];
type AnnotateMaskToleranceKey = keyof AnnotateColorToleranceState;

function cloneAnnotateColorRange(
  range: AnnotateColorRangeState,
): AnnotateColorRangeState {
  return {
    center: {
      ...range.center,
    },
    tolerance: {
      ...range.tolerance,
    },
    sampling: range.sampling,
  };
}

@Component({
  selector: "app-right-panel-annotate-sidebar-section",
  standalone: true,
  changeDetection: ChangeDetectionStrategy.OnPush,
  imports: [FormsModule],
  template: `
    <section class="panel-section">
      <div class="section-header">
        <h2>Annotate Sidebar</h2>
      </div>
      <div class="form-grid">
        <label class="field field-span">
          <span>Loaded Classes</span>
          <input type="text" [value]="annotateClassSummary()" readonly />
        </label>
        <label class="field field-span">
          <span>New Class</span>
          <input
            type="text"
            [ngModel]="annotateCategoryName()"
            (ngModelChange)="annotateCategoryName.set($event)"
          />
        </label>
      </div>
      <div class="action-row annotate-action-row">
        <button
          type="button"
          [disabled]="annotateCategoryName().trim().length === 0"
          (click)="addAnnotateCategory()"
        >
          Add Class
        </button>
      </div>
      <div class="action-row annotate-action-row">
        <button
          type="button"
          [disabled]="sidebarVm()?.canUndo !== true"
          (click)="annotate.undo()"
        >
          Undo
        </button>
        <button
          type="button"
          [disabled]="sidebarVm()?.canRedo !== true"
          (click)="annotate.redo()"
        >
          Redo
        </button>
        <button
          type="button"
          [disabled]="sidebarVm()?.canDeleteSelected !== true"
          (click)="annotate.deleteSelectedObject()"
        >
          Delete Selected
        </button>
        <button
          type="button"
          [disabled]="
            sidebarVm()?.assistAvailable !== true ||
            sidebarVm()?.assistRunning === true
          "
          (click)="annotate.requestAssist()"
        >
          Assist
        </button>
        <button
          type="button"
          [disabled]="sidebarVm()?.canRedrawSelectedBox !== true"
          (click)="annotate.redrawSelectedBox()"
        >
          Redraw Box
        </button>
      </div>
      @if ((sidebarVm()?.categories ?? []).length > 0) {
        <div class="choice-grid">
          @for (category of sidebarVm()?.categories ?? []; track category.id; let categoryIndex = $index) {
            <label class="choice-chip">
              <input
                type="radio"
                name="annotate-category"
                [checked]="annotate.isCategorySelected(categoryIndex)"
                [disabled]="sidebarVm()?.selectedObject == null"
                (change)="annotate.selectCategory(categoryIndex)"
              />
              <span>{{ category.name }}</span>
            </label>
          }
        </div>
      }
      <div class="slot-list annotate-object-list">
        @for (object of sidebarVm()?.objects ?? []; track object.index) {
          <button
            type="button"
            class="slot-card"
            [attr.data-selected]="object.selected"
            (click)="annotate.selectObject(object.index)"
          >
            <strong>{{ object.label }}</strong>
            <p class="section-copy">
              {{ object.selected ? "Selected object" : "Available object" }}
            </p>
          </button>
        }
      </div>
      @if (sidebarVm()?.selectedObject; as selectedObject) {
        <div class="form-grid">
          <label class="field">
            <span>Enabled</span>
            <input
              type="checkbox"
              [ngModel]="selectedObject.enabled"
              (ngModelChange)="updateAnnotateSelectedEnabled($event)"
            />
          </label>
          @if (sidebarVm()?.canEditSelectedMask === true) {
            <label class="field">
              <span>Cleanup Radius</span>
              <input
                type="text"
                inputmode="numeric"
                [ngModel]="annotateMaskCleanupRadius()"
                (ngModelChange)="annotateMaskCleanupRadius.set($event)"
              />
            </label>
            <label class="field field-span">
              <span>Mask Cleanup</span>
              <select
                [ngModel]="annotateMaskCleanupOp()"
                (ngModelChange)="annotateMaskCleanupOp.set($event)"
              >
                @for (option of annotateMaskCleanupOptions; track option.value) {
                  <option [value]="option.value">{{ option.label }}</option>
                }
              </select>
            </label>
          }
        </div>
        @if (sidebarVm()?.canEditSelectedMask === true) {
          <div class="action-row annotate-action-row">
            <button type="button" (click)="applyAnnotateMaskCleanup()">
              Run Mask Cleanup
            </button>
          </div>
          @for (range of annotateMaskRangeOptions; track range.value) {
            <div class="section-header">
              <h3>{{ range.label }} Range</h3>
              <button
                type="button"
                (click)="toggleAnnotateMaskColorSampling(range.value)"
              >
                {{
                  annotateMaskRange(selectedObject, range.value).sampling
                    ? "Cancel Sample"
                    : "Arm Sample"
                }}
              </button>
            </div>
            <div class="form-grid">
              @for (field of annotateMaskCenterFields; track field.value) {
                <label class="field">
                  <span>{{ field.label }}</span>
                  <input
                    type="number"
                    inputmode="decimal"
                    [attr.min]="field.min"
                    [attr.max]="field.max"
                    [attr.step]="field.step"
                    [ngModel]="
                      annotateMaskCenterValue(
                        selectedObject,
                        range.value,
                        field.value
                      )
                    "
                    (ngModelChange)="
                      updateAnnotateMaskCenter(range.value, field.value, $event)
                    "
                  />
                </label>
              }
              @for (field of annotateMaskToleranceFields; track field.value) {
                <label class="field">
                  <span>{{ field.label }}</span>
                  <input
                    type="number"
                    inputmode="decimal"
                    min="0"
                    max="100"
                    step="1"
                    [ngModel]="
                      annotateMaskToleranceValue(
                        selectedObject,
                        range.value,
                        field.value
                      )
                    "
                    (ngModelChange)="
                      updateAnnotateMaskTolerance(range.value, field.value, $event)
                    "
                  />
                </label>
              }
            </div>
            <div class="action-row annotate-action-row">
              <button
                type="button"
                (click)="resetAnnotateMaskColorRange(range.value)"
              >
                Reset {{ range.label }}
              </button>
            </div>
            <p class="section-copy">
              {{ annotateMaskRangeNote(selectedObject, range.value) }}
            </p>
          }
          <p class="section-copy">
            Cleanup now routes through the annotate sidebar bridge. Brush paint,
            color sampling, and dense mask previews still stay with the
            workspace path.
          </p>
        }
      }
      @if (sidebarVm()?.selectedObject?.spline; as spline) {
        <div class="form-grid">
          <label class="field">
            <span>Active Segment</span>
            <select
              [ngModel]="spline.activeSegmentIndex"
              (ngModelChange)="annotate.updateSplineActiveSegment($event)"
            >
              @for (
                segmentIndex of segmentOptions(spline.segmentCount);
                track segmentIndex
              ) {
                <option [ngValue]="segmentIndex">
                  Segment {{ segmentIndex + 1 }}
                </option>
              }
            </select>
          </label>
          <label class="field">
            <span>Handle Mode</span>
            <select
              [ngModel]="spline.activeHandleMode ?? ''"
              [disabled]="!spline.canEditActiveHandleMode"
              (ngModelChange)="annotate.setSplineHandleMode($event)"
            >
              <option value="">Auto</option>
              <option value="corner">Corner</option>
              <option value="smooth">Smooth</option>
              <option value="mirrored">Mirrored</option>
            </select>
          </label>
        </div>
        <div class="action-row annotate-action-row">
          <button
            type="button"
            [disabled]="!spline.canInsertActiveSegmentKnot"
            (click)="annotate.insertSplineKnot()"
          >
            Insert Knot
          </button>
          <button
            type="button"
            [disabled]="!spline.canClose"
            (click)="annotate.closeSpline()"
          >
            Close
          </button>
          <button
            type="button"
            [disabled]="!spline.canReopen"
            (click)="annotate.reopenSpline()"
          >
            Reopen
          </button>
          <button
            type="button"
            [disabled]="!spline.canDeleteActiveKnot"
            (click)="annotate.deleteSplineActiveKnot()"
          >
            Delete Knot
          </button>
        </div>
      }
      @if (sidebarVm()?.selectedObject?.skeleton; as skeleton) {
        @if (skeleton.joints.length > 0) {
          <div class="choice-grid">
            @for (joint of skeleton.joints; track joint.index) {
              <label class="choice-chip">
                <input
                  type="radio"
                  name="annotate-skeleton-joint"
                  [checked]="joint.active"
                  (change)="annotate.updateSkeletonActiveJoint(joint.index)"
                />
                <span>{{ joint.label }}</span>
              </label>
            }
          </div>
        }
        <div class="action-row annotate-action-row">
          <button
            type="button"
            [disabled]="!skeleton.canSkipJoint"
            (click)="annotate.skipSkeletonJoint()"
          >
            Skip Joint
          </button>
          <button
            type="button"
            [disabled]="!skeleton.canHideActiveJoint"
            (click)="annotate.hideSkeletonJoint()"
          >
            Hide Joint
          </button>
          <button
            type="button"
            [disabled]="!skeleton.canReactivateActiveJoint"
            (click)="annotate.showSkeletonJoint()"
          >
            Show Joint
          </button>
          <button
            type="button"
            [disabled]="!skeleton.canReseedActiveJoint"
            (click)="annotate.reseedSkeletonJoint()"
          >
            Reseed Joint
          </button>
        </div>
      }
      <p class="section-copy">{{ annotate.selectedSummary() }}</p>
      <p class="section-copy">{{ annotate.sidebarNote() }}</p>
    </section>
  `,
})
export class RightPanelAnnotateSidebarSectionComponent {
  protected readonly annotate = inject(BrowserAnnotateSidebarState);
  protected readonly sidebarVm = this.annotate.sidebar;
  protected readonly annotateCategoryName = signal("");
  protected readonly annotateMaskCleanupRadius = signal("1");
  protected readonly annotateMaskCleanupOp = signal("largest_component");
  protected readonly annotateMaskRangeOptions = [
    { value: "sup", label: "Suppress" },
    { value: "nosup", label: "No-Suppress" },
  ] as const;
  protected readonly annotateMaskCleanupOptions = [
    { value: "largest_component", label: "Largest Component" },
    { value: "fill_holes", label: "Fill Holes" },
    { value: "dilate", label: "Dilate" },
    { value: "erode", label: "Erode" },
    { value: "open", label: "Open" },
    { value: "close", label: "Close" },
  ] as const;
  protected readonly annotateMaskCenterFields: ReadonlyArray<{
    label: string;
    value: AnnotateMaskCenterKey;
    min: string;
    max: string;
    step: string;
  }> = [
    { label: "Hue", value: "hueDegrees", min: "0", max: "360", step: "1" },
    { label: "Saturation", value: "saturation", min: "0", max: "1", step: "0.01" },
    { label: "Value", value: "value", min: "0", max: "1", step: "0.01" },
  ];
  protected readonly annotateMaskToleranceFields: ReadonlyArray<{
    label: string;
    value: AnnotateMaskToleranceKey;
  }> = [
    { label: "H-", value: "hueMinusPct" },
    { label: "H+", value: "huePlusPct" },
    { label: "S-", value: "saturationMinusPct" },
    { label: "S+", value: "saturationPlusPct" },
    { label: "V-", value: "valueMinusPct" },
    { label: "V+", value: "valuePlusPct" },
  ];

  constructor() {
    effect(() => {
      if (this.annotate.selectedWorkflow() !== "annotate") {
        return;
      }
      const sidebar = this.sidebarVm();
      if (sidebar === null) {
        return;
      }
      const nextRadius = String(sidebar.cleanupRadius);
      if (this.annotateMaskCleanupRadius() !== nextRadius) {
        this.annotateMaskCleanupRadius.set(nextRadius);
      }
    });
  }

  protected segmentOptions(count: number): number[] {
    return Array.from(
      { length: Math.max(0, Math.trunc(count)) },
      (_, index) => index,
    );
  }

  protected annotateClassSummary(): string {
    const categories = this.sidebarVm()?.categories ?? [];
    if (categories.length === 0) {
      return "No classes published yet.";
    }
    return `${categories.length} class${categories.length === 1 ? "" : "es"} loaded`;
  }

  protected addAnnotateCategory(): void {
    const categoryName = this.annotateCategoryName().trim();
    if (categoryName.length === 0) {
      return;
    }
    this.annotate.addCategory(categoryName);
    this.annotateCategoryName.set("");
  }

  protected updateAnnotateSelectedEnabled(enabled: boolean): void {
    const selectedObject = this.sidebarVm()?.selectedObject;
    if (selectedObject === null || selectedObject === undefined) {
      return;
    }
    this.annotate.updateSelectedEnabled(enabled);
  }

  protected applyAnnotateMaskCleanup(): void {
    if (this.sidebarVm()?.canEditSelectedMask !== true) {
      return;
    }
    const parsed = Number(this.annotateMaskCleanupRadius());
    const cleanupRadius = Number.isFinite(parsed)
      ? Math.max(1, Math.min(32, Math.round(parsed)))
      : (this.sidebarVm()?.cleanupRadius ?? 1);
    this.annotateMaskCleanupRadius.set(String(cleanupRadius));
    this.annotate.runMaskCleanup(this.annotateMaskCleanupOp(), cleanupRadius);
  }

  protected annotateMaskRange(
    selectedObject: AnnotateSidebarSelectedObjectState,
    rangeKey: AnnotateMaskRangeKey,
  ): AnnotateColorRangeState {
    return rangeKey === "sup" ? selectedObject.sup : selectedObject.nosup;
  }

  protected annotateMaskCenterValue(
    selectedObject: AnnotateSidebarSelectedObjectState,
    rangeKey: AnnotateMaskRangeKey,
    field: AnnotateMaskCenterKey,
  ): number {
    return this.annotateMaskRange(selectedObject, rangeKey).center[field];
  }

  protected annotateMaskToleranceValue(
    selectedObject: AnnotateSidebarSelectedObjectState,
    rangeKey: AnnotateMaskRangeKey,
    field: AnnotateMaskToleranceKey,
  ): number {
    return this.annotateMaskRange(selectedObject, rangeKey).tolerance[field];
  }

  protected annotateMaskRangeNote(
    selectedObject: AnnotateSidebarSelectedObjectState,
    rangeKey: AnnotateMaskRangeKey,
  ): string {
    return this.annotateMaskRange(selectedObject, rangeKey).sampling
      ? `Workspace sampling is armed for the ${rangeKey === "sup" ? "suppress" : "no-suppress"} range. Click the capture to sample a color.`
      : `Arm sampling to pick a ${rangeKey === "sup" ? "suppress" : "no-suppress"} color from the workspace.`;
  }

  protected toggleAnnotateMaskColorSampling(
    rangeKey: AnnotateMaskRangeKey,
  ): void {
    this.dispatchAnnotateMaskRangeUpdate((sup, nosup) => ({
      sup: {
        ...sup,
        sampling: rangeKey === "sup" ? !sup.sampling : false,
      },
      nosup: {
        ...nosup,
        sampling: rangeKey === "nosup" ? !nosup.sampling : false,
      },
    }));
  }

  protected resetAnnotateMaskColorRange(rangeKey: AnnotateMaskRangeKey): void {
    this.dispatchAnnotateMaskRangeUpdate((sup, nosup) => ({
      sup: rangeKey === "sup" ? defaultAnnotateColorRangeState() : sup,
      nosup: rangeKey === "nosup" ? defaultAnnotateColorRangeState() : nosup,
    }));
  }

  protected updateAnnotateMaskCenter(
    rangeKey: AnnotateMaskRangeKey,
    field: AnnotateMaskCenterKey,
    value: unknown,
  ): void {
    this.dispatchAnnotateMaskRangeUpdate((sup, nosup) => {
      const current = rangeKey === "sup" ? sup : nosup;
      const nextValue =
        field === "hueDegrees"
          ? this.coerceMaskNumber(value, current.center.hueDegrees, 0, 360, 1)
          : this.coerceMaskNumber(value, current.center[field], 0, 1, 0.01);
      const nextRange: AnnotateColorRangeState = {
        ...current,
        center: {
          ...current.center,
          [field]: nextValue,
        },
      };
      return {
        sup: rangeKey === "sup" ? nextRange : sup,
        nosup: rangeKey === "nosup" ? nextRange : nosup,
      };
    });
  }

  protected updateAnnotateMaskTolerance(
    rangeKey: AnnotateMaskRangeKey,
    field: AnnotateMaskToleranceKey,
    value: unknown,
  ): void {
    this.dispatchAnnotateMaskRangeUpdate((sup, nosup) => {
      const current = rangeKey === "sup" ? sup : nosup;
      const nextRange: AnnotateColorRangeState = {
        ...current,
        tolerance: {
          ...current.tolerance,
          [field]: this.coerceMaskNumber(
            value,
            current.tolerance[field],
            0,
            100,
            1,
          ),
        },
      };
      return {
        sup: rangeKey === "sup" ? nextRange : sup,
        nosup: rangeKey === "nosup" ? nextRange : nosup,
      };
    });
  }

  private dispatchAnnotateMaskRangeUpdate(
    update: (
      sup: AnnotateColorRangeState,
      nosup: AnnotateColorRangeState,
    ) => {
      sup: AnnotateColorRangeState;
      nosup: AnnotateColorRangeState;
    },
  ): void {
    const selectedObject = this.sidebarVm()?.selectedObject;
    if (selectedObject === null || selectedObject === undefined) {
      return;
    }
    const { sup, nosup } = update(
      cloneAnnotateColorRange(selectedObject.sup),
      cloneAnnotateColorRange(selectedObject.nosup),
    );
    this.annotate.updateMaskColorRanges(sup, nosup);
  }

  private coerceMaskNumber(
    value: unknown,
    fallback: number,
    min: number,
    max: number,
    step: number,
  ): number {
    const parsed = Number(value);
    if (!Number.isFinite(parsed)) {
      return fallback;
    }
    const clamped = Math.min(max, Math.max(min, parsed));
    if (step >= 1) {
      return Math.round(clamped);
    }
    return Number(clamped.toFixed(2));
  }
}

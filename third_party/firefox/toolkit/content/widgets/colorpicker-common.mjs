/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// @ts-nocheck Do this after migration from devtools

const lazy = {};
ChromeUtils.defineLazyGetter(lazy, "l10n", function () {
  return new Localization(["devtools/client/inspector.ftl"], true);
});

const ARROW_KEYS = ["ArrowUp", "ArrowRight", "ArrowDown", "ArrowLeft"];
const [ArrowUp, ArrowRight, ArrowDown, ArrowLeft] = ARROW_KEYS;
const SLIDER = {
  hue: {
    MIN: "0",
    MAX: "128",
    STEP: "1",
  },
  alpha: {
    MIN: "0",
    MAX: "1",
    STEP: "0.01",
  },
};

export class ColorPickerCommon {
  constructor(element) {
    this.document = element.ownerDocument;
    this.element = element;

    this.element.className = "spectrum-container";

    this.onElementClick = this.onElementClick.bind(this);
    this.element.addEventListener("click", this.onElementClick);

    this.dragger = this.element.querySelector(".spectrum-color");
    this.dragger.title = lazy.l10n.formatValueSync(
      "colorpicker-tooltip-spectrum-dragger-title"
    );

    this.dragHelper = this.element.querySelector(".spectrum-dragger");
    draggable(this.dragger, this.dragHelper, this.onDraggerMove.bind(this));

    this.controls = this.element.querySelector(".spectrum-controls");
    this.colorPreview = this.element.querySelector(".spectrum-color-preview");

    this.hueSlider = this.createSlider("hue", this.onHueSliderMove.bind(this));
    this.hueSlider.setAttribute("aria-describedby", this.dragHelper.id);
    this.alphaSlider = this.createSlider(
      "alpha",
      this.onAlphaSliderMove.bind(this)
    );
  }

  set rgb([r, g, b, a]) {
    this.rgbFloat = [r / 255, g / 255, b / 255, a];
  }

  set rgbFloat([r, g, b, a]) {
    this.hsv = [...InspectorUtils.rgbToHsv(r, g, b), a];
  }

  #toRgbInt(rgbFloat) {
    return rgbFloat.map(c => Math.round(c * 255));
  }

  get rgbFloat() {
    const [h, s, v, a] = this.hsv;
    return [...InspectorUtils.hsvToRgb(h, s, v), a];
  }

  get rgb() {
    const [r, g, b, a] = this.rgbFloat;
    return [...this.#toRgbInt([r, g, b]), a];
  }

  get colorName() {
    const [r, g, b] = this.rgbFloat;
    const { exact, colorName } = InspectorUtils.rgbToNearestColorName(r, g, b);
    return exact
      ? colorName
      : lazy.l10n.formatValueSync("colorpicker-tooltip-color-name-title", {
          colorName,
        });
  }

  get rgbNoSatVal() {
    return [
      ...this.#toRgbInt(InspectorUtils.hsvToRgb(this.hsv[0], 1, 1)),
      this.hsv[3],
    ];
  }

  get rgbCssString() {
    const rgb = this.rgb;
    return (
      "rgba(" + rgb[0] + ", " + rgb[1] + ", " + rgb[2] + ", " + rgb[3] + ")"
    );
  }

  show() {
    this.dragWidth = this.dragger.offsetWidth;
    this.dragHeight = this.dragger.offsetHeight;
    this.dragHelperHeight = this.dragHelper.offsetHeight;
    this.dragger.focus({ focusVisible: false });

    this.updateUI();
  }

  enableAlphaPicker() {
    this.element.querySelector(".spectrum-alpha").hidden = false;
  }

  onElementClick(e) {
    e.stopPropagation();
  }

  onHueSliderMove() {
    this.hsv[0] = this.hueSlider.value / this.hueSlider.max;
    this.updateUI();
    this.onChange();
  }

  onDraggerMove(dragX, dragY) {
    this.hsv[1] = dragX / this.dragWidth;
    this.hsv[2] = (this.dragHeight - dragY) / this.dragHeight;
    this.updateUI();
    this.onChange();
  }

  onAlphaSliderMove() {
    this.hsv[3] = this.alphaSlider.value / this.alphaSlider.max;
    this.updateUI();
    this.onChange();
  }

  onChange() {
    throw new Error("Not implemented");
  }

  createSlider(sliderType, onSliderMove) {
    const container = this.element.querySelector(`.spectrum-${sliderType}`);

    const slider = this.document.createElement("input");
    slider.className = `spectrum-${sliderType}-input`;
    slider.type = "range";
    slider.min = SLIDER[sliderType].MIN;
    slider.max = SLIDER[sliderType].MAX;
    slider.step = SLIDER[sliderType].STEP;
    slider.title = lazy.l10n.formatValueSync(
      `colorpicker-tooltip-${sliderType}-slider-title`
    );
    slider.addEventListener("input", onSliderMove);

    container.appendChild(slider);
    return slider;
  }

  updateAlphaSlider() {
    const rgb = this.rgb;

    const rgbNoAlpha = "rgb(" + rgb[0] + "," + rgb[1] + "," + rgb[2] + ")";
    const rgbAlpha0 = "rgba(" + rgb[0] + "," + rgb[1] + "," + rgb[2] + ", 0)";
    const alphaGradient =
      "linear-gradient(to right, " + rgbAlpha0 + ", " + rgbNoAlpha + ")";
    this.alphaSlider.style.background = alphaGradient;
  }

  updateColorPreview() {
    this.colorPreview.style.setProperty("--overlay-color", this.rgbCssString);

    this.colorPreview.title = this.colorName;
  }

  updateDragger() {
    const flatColor =
      "rgb(" +
      this.rgbNoSatVal[0] +
      ", " +
      this.rgbNoSatVal[1] +
      ", " +
      this.rgbNoSatVal[2] +
      ")";
    this.dragger.style.backgroundColor = flatColor;

    this.dragger.setAttribute("aria-valuetext", this.rgbCssString);
  }

  updateHueSlider() {
    this.hueSlider.setAttribute("aria-valuetext", this.rgbCssString);
  }

  updateHelperLocations() {
    const h = this.hsv[0];
    const s = this.hsv[1];
    const v = this.hsv[2];

    let dragX = s * this.dragWidth;
    let dragY = this.dragHeight - v * this.dragHeight;
    const helperDim = this.dragHelperHeight / 2;

    dragX = Math.max(
      -helperDim,
      Math.min(this.dragWidth - helperDim, dragX - helperDim)
    );
    dragY = Math.max(
      -helperDim,
      Math.min(this.dragHeight - helperDim, dragY - helperDim)
    );

    this.dragHelper.style.top = dragY + "px";
    this.dragHelper.style.left = dragX + "px";

    this.hueSlider.value = h * this.hueSlider.max;

    this.alphaSlider.value = this.hsv[3] * this.alphaSlider.max;
  }

  updateUI() {
    this.updateHelperLocations();

    this.updateColorPreview();
    this.updateDragger();
    this.updateHueSlider();
    this.updateAlphaSlider();
  }

  destroy() {
    this.element.removeEventListener("click", this.onElementClick);
    this.hueSlider.removeEventListener("input", this.onHueSliderMove);
    this.alphaSlider.removeEventListener("input", this.onAlphaSliderMove);

    this.element.remove();

    this.dragger = this.dragHelper = null;
    this.alphaSlider = null;
    this.hueSlider = null;
    this.colorPreview = null;
    this.element = null;
  }
}

function draggable(element, dragHelper, onmove) {
  const doc = element.ownerDocument;
  let dragging = false;
  let offset = {};
  let maxHeight = 0;
  let maxWidth = 0;

  function setDraggerDimensionsAndOffset() {
    maxHeight = element.offsetHeight;
    maxWidth = element.offsetWidth;
    offset = element.getBoundingClientRect();
  }

  function prevent(e) {
    e.stopPropagation();
    e.preventDefault();
  }

  function move(e) {
    if (dragging) {
      if (e.buttons === 0) {
        stop();
        return;
      }
      const pageX = e.pageX;
      const pageY = e.pageY;

      const dragX = Math.max(0, Math.min(pageX - offset.left, maxWidth));
      const dragY = Math.max(0, Math.min(pageY - offset.top, maxHeight));

      onmove.apply(element, [dragX, dragY]);
    }
  }

  function start(e) {
    const rightClick = e.which === 3;

    if (!rightClick && !dragging) {
      dragging = true;
      setDraggerDimensionsAndOffset();

      move(e);

      doc.addEventListener("selectstart", prevent);
      doc.addEventListener("dragstart", prevent);
      doc.addEventListener("mousemove", move);
      doc.addEventListener("mouseup", stop);

      prevent(e);
    }
  }

  function stop() {
    if (dragging) {
      doc.removeEventListener("selectstart", prevent);
      doc.removeEventListener("dragstart", prevent);
      doc.removeEventListener("mousemove", move);
      doc.removeEventListener("mouseup", stop);
    }
    dragging = false;
  }

  function onKeydown(e) {
    const { key } = e;

    if (!ARROW_KEYS.includes(key)) {
      return;
    }

    setDraggerDimensionsAndOffset();
    const { offsetHeight, offsetTop, offsetLeft } = dragHelper;
    let dragX = offsetLeft + offsetHeight / 2;
    let dragY = offsetTop + offsetHeight / 2;

    if (key === ArrowLeft && dragX > 0) {
      dragX -= 1;
    } else if (key === ArrowRight && dragX < maxWidth) {
      dragX += 1;
    } else if (key === ArrowUp && dragY > 0) {
      dragY -= 1;
    } else if (key === ArrowDown && dragY < maxHeight) {
      dragY += 1;
    }

    onmove.apply(element, [dragX, dragY]);
  }

  element.addEventListener("mousedown", start);
  element.addEventListener("keydown", onKeydown);
}

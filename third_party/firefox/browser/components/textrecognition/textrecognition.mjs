/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

window.docShell.chromeEventHandler.classList.add("textRecognitionDialogFrame");

window.addEventListener("DOMContentLoaded", () => {
  new TextRecognitionModal(...window.arguments);
});


class TextRecognitionModal {
  constructor(resultsPromise, resizeVertically, openLinkIn, timerId) {
    this.textEl = document.querySelector(".textRecognitionText");

    this.headerEls = document.querySelectorAll(".textRecognitionHeader");

    this.linkEl = document.querySelector(
      "#text-recognition-header-no-results a"
    );

    this.resizeVertically = resizeVertically;
    this.openLinkIn = openLinkIn;
    this.setupLink();
    this.setupCloseHandler();

    this.showHeaderByID("text-recognition-header-loading");

    resultsPromise.then(
      ({ results, direction }) => {
        if (results.length === 0) {
          this.showHeaderByID("text-recognition-header-no-results");
          return;
        }

        this.runClusteringAndUpdateUI(results, direction);
        this.showHeaderByID("text-recognition-header-results");

      },
      error => {
        this.showHeaderByID("text-recognition-header-no-results");

        console.error(
          "There was an error recognizing the text from an image.",
          error
        );
      }
    );
  }

  setupCloseHandler() {
    document
      .querySelector("#text-recognition-close")
      .addEventListener("click", () => {
        window.close();
      });
  }

  setupLink() {
    this.linkEl.href = Services.urlFormatter.formatURL(this.linkEl.href);
    this.linkEl.addEventListener("click", event => {
      event.preventDefault();
      this.openLinkIn(this.linkEl.href, "tab", {
        forceForeground: true,
        triggeringPrincipal:
          Services.scriptSecurityManager.getSystemPrincipal(),
      });
    });
  }

  showHeaderByID(id) {
    for (const header of this.headerEls) {
      header.style.display = "none";
    }

    document.getElementById(id).style.display = "";
    this.resizeVertically();
  }

  static copy(text) {
    const clipboard = Cc["@mozilla.org/widget/clipboardhelper;1"].getService(
      Ci.nsIClipboardHelper
    );
    clipboard.copyString(text);
  }

  runClusteringAndUpdateUI(results, direction) {
    const centers = [];

    for (const result of results) {
      const p = result.quad;

      const minOrMax = direction === "ltr" ? Math.min : Math.max;

      centers.push([
        minOrMax(p.p1.x, p.p2.x, p.p3.x, p.p4.x),
        (p.p1.y, p.p2.y, p.p3.y, p.p4.y) / 4,
      ]);
    }

    const distSq = new DistanceSquared(centers);

    const averageDistance = Math.sqrt(distSq.quantile(0.2));
    const clusters = densityCluster(
      centers,
      averageDistance,
      2
    );

    let text = "";
    for (const cluster of clusters) {
      const pCluster = document.createElement("p");
      pCluster.className = "textRecognitionTextCluster";

      for (let i = 0; i < cluster.length; i++) {
        const index = cluster[i];
        const { string } = results[index];
        if (i + 1 === cluster.length) {
          text += string + "\n\n";
          pCluster.innerText += string;
        } else {
          text += string + " ";
          pCluster.innerText += string + " ";
        }
      }
      this.textEl.appendChild(pCluster);
    }

    this.textEl.style.display = "block";

    text = text.trim();
    TextRecognitionModal.copy(text);
  }
}



function densityCluster(points, distance, minPoints) {
  const labels = Array(points.length);
  const noiseLabel = "noise";

  let nextClusterIndex = 0;

  for (let pointIndex = 0; pointIndex < points.length; pointIndex++) {
    if (labels[pointIndex] !== undefined) {
      continue;
    }

    const neighbors = getNeighborsWithinDistance(points, distance, pointIndex);

    if (neighbors.length < minPoints) {
      labels[pointIndex] = noiseLabel;
      continue;
    }

    const clusterIndex = nextClusterIndex++;
    labels[pointIndex] = clusterIndex;

    for (let i = 0; i < neighbors.length; i++) {
      const nextPointIndex = neighbors[i];
      if (typeof labels[nextPointIndex] === "number") {
        continue;
      }

      if (labels[nextPointIndex] === noiseLabel) {
        labels[nextPointIndex] = clusterIndex;
        continue;
      }

      labels[nextPointIndex] = clusterIndex;

      const newNeighbors = getNeighborsWithinDistance(
        points,
        distance,
        nextPointIndex
      );

      if (newNeighbors.length >= minPoints) {
        for (const newNeighbor of newNeighbors) {
          if (!neighbors.includes(newNeighbor)) {
            neighbors.push(newNeighbor);
          }
        }
      }
    }
  }

  const clusters = [];

  for (let i = 0; i < nextClusterIndex; i++) {
    clusters[i] = [];
  }

  for (let pointIndex = 0; pointIndex < labels.length; pointIndex++) {
    const label = labels[pointIndex];
    if (typeof label === "number") {
      clusters[label].push(pointIndex);
    } else if (label === noiseLabel) {
      clusters.push([pointIndex]);
    } else {
      throw new Error("Logic error. Expected every point to have a label.");
    }
  }

  clusters.sort((a, b) => points[b[0]][1] - points[a[0]][1]);

  return clusters;
}

function getNeighborsWithinDistance(points, distance, index) {
  let neighbors = [index];
  const distanceSquared = distance * distance;

  for (let otherIndex = 0; otherIndex < points.length; otherIndex++) {
    if (otherIndex === index) {
      continue;
    }
    const a = points[index];
    const b = points[otherIndex];
    const dx = a[0] - b[0];
    const dy = a[1] - b[1];

    if (dx * dx + dy * dy < distanceSquared) {
      neighbors.push(otherIndex);
    }
  }

  return neighbors;
}

class DistanceSquared {
  #distances = new Map();
  #list;
  #distancesSorted;

  constructor(list) {
    this.#list = list;
    for (let aIndex = 0; aIndex < list.length; aIndex++) {
      for (let bIndex = aIndex + 1; bIndex < list.length; bIndex++) {
        const id = this.#getTupleID(aIndex, bIndex);
        const a = this.#list[aIndex];
        const b = this.#list[bIndex];
        const dx = a[0] - b[0];
        const dy = a[1] - b[1];
        this.#distances.set(id, dx * dx + dy * dy);
      }
    }
  }

  #getTupleID(aIndex, bIndex) {
    return aIndex < bIndex
      ? aIndex * this.#list.length + bIndex
      : bIndex * this.#list.length + aIndex;
  }

  get(aIndex, bIndex) {
    return this.#distances.get(this.#getTupleID(aIndex, bIndex));
  }

  quantile(percentile) {
    if (!this.#distancesSorted) {
      this.#distancesSorted = [...this.#distances.values()].sort(
        (a, b) => a - b
      );
    }
    const index = Math.max(
      0,
      Math.min(
        this.#distancesSorted.length - 1,
        Math.round(this.#distancesSorted.length * percentile)
      )
    );
    return this.#distancesSorted[index];
  }
}

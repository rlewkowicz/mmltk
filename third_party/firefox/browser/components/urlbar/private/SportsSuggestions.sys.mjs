/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { RealtimeSuggestProvider } from "moz-src:///browser/components/urlbar/private/RealtimeSuggestProvider.sys.mjs";

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  ObjectUtils: "resource://gre/modules/ObjectUtils.sys.mjs",
  UrlbarUtils: "moz-src:///browser/components/urlbar/UrlbarUtils.sys.mjs",
});

export class SportsSuggestions extends RealtimeSuggestProvider {
  get realtimeType() {
    return "sports";
  }

  get isSponsored() {
    return false;
  }

  get merinoProvider() {
    return "sports";
  }

  getViewTemplateForImageContainer(item, index) {
    return ["home", "away"].map(team => ({
      name: `${team}-team-image-container-${index}`,
      tag: "span",
      classList: ["urlbarView-realtime-image-container"],
      children: [
        {
          name: `${team}-team-image-${index}`,
          tag: "img",
          classList: ["urlbarView-realtime-image"],
        },
        {
          name: `${team}-team-date-chiclet-day-${index}`,
          tag: "span",
          classList: ["urlbarView-sports-date-chiclet-day"],
        },
        {
          name: `${team}-team-date-chiclet-month-${index}`,
          tag: "span",
          classList: ["urlbarView-sports-date-chiclet-month"],
        },
      ],
    }));
  }

  getViewTemplateForDescriptionTop(item, index) {
    return stringifiedScore(item.home_team.score) &&
      stringifiedScore(item.away_team.score)
      ? this.#viewTemplateTopWithScores(index)
      : this.#viewTemplateTopWithoutScores(index);
  }

  #viewTemplateTopWithScores(index) {
    return [
      {
        name: `home-team-name-${index}`,
        tag: "span",
      },
      {
        name: `home-team-score-${index}`,
        tag: "span",
        classList: ["urlbarView-sports-score", "urlbarView-sports-score-home"],
      },
      {
        tag: "span",
        classList: ["urlbarView-realtime-description-separator-dot"],
      },
      {
        name: `away-team-score-${index}`,
        tag: "span",
        classList: ["urlbarView-sports-score", "urlbarView-sports-score-away"],
      },
      {
        name: `away-team-name-${index}`,
        tag: "span",
      },
    ];
  }

  #viewTemplateTopWithoutScores(index) {
    return [
      {
        name: `team-names-${index}`,
        tag: "span",
        classList: ["urlbarView-sports-team-names"],
      },
    ];
  }

  getViewTemplateForDescriptionBottom(item, index) {
    return [
      {
        name: `sport-${index}`,
        tag: "span",
      },
      {
        tag: "span",
        classList: ["urlbarView-realtime-description-separator-dot"],
      },
      {
        name: `date-${index}`,
        tag: "span",
      },
      {
        tag: "span",
        classList: ["urlbarView-realtime-description-separator-dot"],
      },
      {
        name: `status-${index}`,
        tag: "span",
        classList: ["urlbarView-sports-status"],
      },
    ];
  }

  getViewUpdateForPayloadItem(item, index) {
    let topUpdate =
      stringifiedScore(item.home_team.score) &&
      stringifiedScore(item.away_team.score)
        ? this.#viewUpdateTopWithScores(item, index)
        : this.#viewUpdateTopWithoutScores(item, index);

    return {
      ...topUpdate,
      ...this.#viewUpdateImageAndBottom(item, index),
      [`item_${index}`]: {
        attributes: {
          "sport-category": item.sport_category,
          status: item.status_type,
        },
      },
    };
  }

  #viewUpdateTopWithScores(item, i) {
    return {
      [`home-team-name-${i}`]: {
        textContent: item.home_team.name,
      },
      [`home-team-score-${i}`]: {
        textContent: stringifiedScore(item.home_team.score),
      },
      [`away-team-name-${i}`]: {
        textContent: item.away_team.name,
      },
      [`away-team-score-${i}`]: {
        textContent: stringifiedScore(item.away_team.score),
      },
    };
  }

  #viewUpdateTopWithoutScores(item, i) {
    return {
      [`team-names-${i}`]: {
        l10n: {
          id: "urlbar-result-sports-team-names",
          args: {
            homeTeam: item.home_team.name,
            awayTeam: item.away_team.name,
          },
        },
      },
    };
  }

  #viewUpdateImageAndBottom(item, i) {
    let date = new Date(item.date);
    let {
      formattedDate,
      formattedTime,
      parseDateResult: { daysUntil, zonedNow },
    } = lazy.UrlbarUtils.formatDate(date, {
      forceAbsoluteDate: !!item.home_team?.icon || !!item.away_team?.icon,
      includeTimeZone: true,
      capitalizeRelativeDate: true,
    });

    let partsArray = new Intl.DateTimeFormat(undefined, {
      month: "short",
      day: "numeric",
      timeZone: zonedNow.timeZoneId,
    }).formatToParts(date);
    let partsMap = Object.fromEntries(
      partsArray.map(({ type, value }) => [type, value])
    );

    let imageUpdatesByTeam = ["home", "away"].reduce((memo, team) => {
      let itemKey = `${team}_team`;
      let icon = item[itemKey]?.icon;
      memo[team] = {
        [`image-container-${i}`]: {
          attributes: {
            "has-team-icon": icon ? "" : null,
          },
        },
        [`image-${i}`]: {
          attributes: {
            src: icon ?? null,
          },
        },
        [`date-chiclet-day-${i}`]: {
          textContent: partsMap.day ?? "",
        },
        [`date-chiclet-month-${i}`]: {
          textContent: partsMap.month ?? "",
        },
      };
      return memo;
    }, {});

    // @ts-ignore
    imageUpdatesByTeam.away[`image-container-${i}`].attributes.hidden =
      lazy.ObjectUtils.deepEqual(
        // @ts-ignore
        imageUpdatesByTeam.home,
        // @ts-ignore
        imageUpdatesByTeam.away
      )
        ? ""
        : null;

    let imageUpdate = Object.entries(imageUpdatesByTeam).reduce(
      (memo, [team, teamUpdate]) => {
        for (let [key, value] of Object.entries(teamUpdate)) {
          memo[`${team}-team-${key}`] = value;
        }
        return memo;
      },
      {}
    );

    let dateUpdate;
    if (formattedTime) {
      dateUpdate = {
        [`date-${i}`]: {
          l10n: {
            id: "urlbar-result-sports-game-date-with-time",
            args: {
              date: formattedDate,
              time: formattedTime,
            },
          },
        },
      };
    } else {
      dateUpdate = {
        [`date-${i}`]: {
          textContent: formattedDate,
        },
      };
    }

    let statusUpdate;
    if (item.status_type == "live") {
      statusUpdate = {
        [`status-${i}`]: {
          l10n: {
            id: "urlbar-result-sports-status-live",
          },
        },
      };
    } else if (daysUntil == 0 && item.status_type == "past") {
      statusUpdate = {
        [`status-${i}`]: {
          l10n: {
            id: "urlbar-result-sports-status-final",
          },
        },
      };
    } else {
      statusUpdate = {
        [`status-${i}`]: {
          textContent: "",
        },
      };
    }

    return {
      ...imageUpdate,
      ...dateUpdate,
      ...statusUpdate,
      [`sport-${i}`]: {
        textContent: item.sport,
      },
    };
  }
}

function stringifiedScore(scoreValue) {
  let s = scoreValue;
  if (typeof s == "number") {
    s = String(s);
  }
  return typeof s == "string" ? s : "";
}

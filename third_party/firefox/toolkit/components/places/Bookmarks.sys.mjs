/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */



const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  PlacesUtils: "resource://gre/modules/PlacesUtils.sys.mjs",
});

const MATCH_ANYWHERE_UNMODIFIED =
  Ci.mozIPlacesAutoComplete.MATCH_ANYWHERE_UNMODIFIED;
const BEHAVIOR_BOOKMARK = Ci.mozIPlacesAutoComplete.BEHAVIOR_BOOKMARK;

export var Bookmarks = Object.freeze({
  TYPE_BOOKMARK: 1,
  TYPE_FOLDER: 2,
  TYPE_SEPARATOR: 3,

  DEFAULT_INDEX: -1,

  MAX_TAG_LENGTH: 100,

  SOURCES: {
    DEFAULT: Ci.nsINavBookmarksService.SOURCE_DEFAULT,
    IMPORT: Ci.nsINavBookmarksService.SOURCE_IMPORT,
    RESTORE: Ci.nsINavBookmarksService.SOURCE_RESTORE,
    RESTORE_ON_STARTUP: Ci.nsINavBookmarksService.SOURCE_RESTORE_ON_STARTUP,
  },

  rootGuid: "root________",
  menuGuid: "menu________",
  toolbarGuid: "toolbar_____",
  unfiledGuid: "unfiled_____",
  mobileGuid: "mobile______",

  tagsGuid: "tags________",

  userContentRoots: [
    "toolbar_____",
    "menu________",
    "unfiled_____",
    "mobile______",
  ],

  unsavedGuid: "new_________",

  virtualMenuGuid: "menu_______v",
  virtualToolbarGuid: "toolbar____v",
  virtualUnfiledGuid: "unfiled____v",
  virtualMobileGuid: "mobile_____v",

  isVirtualRootItem(guid) {
    return (
      guid == lazy.PlacesUtils.bookmarks.virtualMenuGuid ||
      guid == lazy.PlacesUtils.bookmarks.virtualToolbarGuid ||
      guid == lazy.PlacesUtils.bookmarks.virtualUnfiledGuid ||
      guid == lazy.PlacesUtils.bookmarks.virtualMobileGuid
    );
  },

  createVirtualLinkToRoot({ guid, title }) {
    return {
      type: lazy.PlacesUtils.TYPE_X_MOZ_PLACE,
      itemGuid: lazy.PlacesUtils.bookmarks._virtualGuidForRoot(guid),
      concreteGuid: guid,
      uri: `place:parent=${guid}`,
      instanceId: lazy.PlacesUtils.instanceId,
      title,
    };
  },

  _virtualGuidForRoot(guid) {
    switch (guid) {
      case lazy.PlacesUtils.bookmarks.menuGuid:
        return lazy.PlacesUtils.bookmarks.virtualMenuGuid;
      case lazy.PlacesUtils.bookmarks.toolbarGuid:
        return lazy.PlacesUtils.bookmarks.virtualToolbarGuid;
      case lazy.PlacesUtils.bookmarks.unfiledGuid:
        return lazy.PlacesUtils.bookmarks.virtualUnfiledGuid;
      case lazy.PlacesUtils.bookmarks.mobileGuid:
        return lazy.PlacesUtils.bookmarks.virtualMobileGuid;
      default:
        return null;
    }
  },

  getLocalizedTitle(info) {
    if (!lazy.PlacesUtils.bookmarks.userContentRoots.includes(info.guid)) {
      return info.title;
    }

    switch (info.guid) {
      case lazy.PlacesUtils.bookmarks.toolbarGuid:
        return lazy.PlacesUtils.getString("BookmarksToolbarFolderTitle");
      case lazy.PlacesUtils.bookmarks.menuGuid:
        return lazy.PlacesUtils.getString("BookmarksMenuFolderTitle");
      case lazy.PlacesUtils.bookmarks.unfiledGuid:
        return lazy.PlacesUtils.getString("OtherBookmarksFolderTitle");
      case lazy.PlacesUtils.bookmarks.mobileGuid:
        return lazy.PlacesUtils.getString("MobileBookmarksFolderTitle");
      default:
        throw new Error(
          `Unsupported guid ${info.guid} passed to getLocalizedTitle!`
        );
    }
  },

  insert(info) {
    let now = new Date();
    let addedTime = (info && info.dateAdded) || now;
    let modTime = addedTime;
    if (addedTime > now) {
      modTime = now;
    }
    let insertInfo = validateBookmarkObject("Bookmarks.sys.mjs: insert", info, {
      type: { defaultValue: this.TYPE_BOOKMARK },
      index: { defaultValue: this.DEFAULT_INDEX },
      url: {
        requiredIf: b => b.type == this.TYPE_BOOKMARK,
        validIf: b => b.type == this.TYPE_BOOKMARK,
      },
      parentGuid: {
        required: true,
        validIf: b =>
          false || b.parentGuid != this.rootGuid,
      },
      title: {
        defaultValue: "",
        validIf: b =>
          b.type == this.TYPE_BOOKMARK ||
          b.type == this.TYPE_FOLDER ||
          b.title === "",
      },
      dateAdded: { defaultValue: addedTime },
      lastModified: {
        defaultValue: modTime,
        validIf: b =>
          b.lastModified >= now ||
          (b.dateAdded && b.lastModified >= b.dateAdded),
      },
      source: { defaultValue: this.SOURCES.DEFAULT },
    });

    return (async () => {
      let parent = await fetchBookmark({ guid: insertInfo.parentGuid });
      if (!parent) {
        throw new Error("parentGuid must be valid");
      }

      if (
        insertInfo.index == this.DEFAULT_INDEX ||
        insertInfo.index > parent._childCount
      ) {
        insertInfo.index = parent._childCount;
      }

      let item = await insertBookmark(insertInfo, parent);
      let itemDetailMap = await getBookmarkDetailMap([item.guid]);
      let itemDetail = itemDetailMap.get(item.guid);

      let isTagging = parent._parentId == lazy.PlacesUtils.tagsFolderId;
      let isTagsFolder = parent._id == lazy.PlacesUtils.tagsFolderId;
      let url = "";
      if (item.type == Bookmarks.TYPE_BOOKMARK) {
        url = item.url.href;
      }

      const notifications = [
        new PlacesBookmarkAddition({
          id: itemDetail.id,
          url,
          itemType: item.type,
          parentId: parent._id,
          index: item.index,
          title: item.title,
          dateAdded: item.dateAdded,
          guid: item.guid,
          parentGuid: item.parentGuid,
          source: item.source,
          isTagging: isTagging || isTagsFolder,
          tags: itemDetail.tags,
          frecency: itemDetail.frecency,
          hidden: itemDetail.hidden,
          visitCount: itemDetail.visitCount,
          lastVisitDate: itemDetail.lastVisitDate,
          targetFolderGuid: itemDetail.targetFolderGuid,
          targetFolderItemId: itemDetail.targetFolderItemId,
          targetFolderTitle: itemDetail.targetFolderTitle,
        }),
      ];

      if (isTagging) {
        for (let entry of await fetchBookmarksByURL(item, {
          concurrent: true,
        })) {
          notifications.push(
            new PlacesBookmarkTags({
              id: entry._id,
              itemType: entry.type,
              url,
              guid: entry.guid,
              parentGuid: entry.parentGuid,
              tags: entry._tags,
              lastModified: entry.lastModified,
              source: item.source,
              isTagging: false,
            })
          );
        }
      }

      PlacesObservers.notifyListeners(notifications);

      delete item.source;
      return Object.assign({}, item);
    })();
  },

  insertTree(tree, options) {
    if (!tree || typeof tree != "object") {
      throw new Error("Should be provided a valid tree object.");
    }
    if (!Array.isArray(tree.children) || !tree.children.length) {
      throw new Error("Should have a non-zero number of children to insert.");
    }
    if (!lazy.PlacesUtils.isValidGuid(tree.guid)) {
      throw new Error(
        `The parent guid is not valid (${tree.guid} ${tree.title}).`
      );
    }
    if (tree.guid == this.rootGuid) {
      throw new Error("Can't insert into the root.");
    }
    if (tree.guid == this.tagsGuid) {
      throw new Error("Can't use insertTree to insert tags.");
    }
    if (
      tree.hasOwnProperty("source") &&
      !Object.values(this.SOURCES).includes(tree.source)
    ) {
      throw new Error("Can't use source value " + tree.source);
    }
    if (options && typeof options != "object") {
      throw new Error("Options should be a valid object");
    }
    let fixupOrSkipInvalidEntries =
      options && !!options.fixupOrSkipInvalidEntries;

    let insertInfos = [];
    let urlsThatMightNeedPlaces = [];

    let fallbackLastAdded = new Date();

    const { TYPE_BOOKMARK, TYPE_FOLDER, SOURCES } = this;

    let source = tree.source || SOURCES.DEFAULT;

    function appendInsertionInfoForInfoArray(infos, indexToUse, parentGuid) {
      let shouldUseNullIndices = false;
      if (indexToUse === null) {
        shouldUseNullIndices = true;
        indexToUse = 0;
      }

      let lastAddedForParent = new Date(0);
      for (let info of infos) {
        let time = (info && info.dateAdded) || fallbackLastAdded;
        let validationSchema = {
          guid: { defaultValue: lazy.PlacesUtils.history.makeGuid() },
          type: { defaultValue: TYPE_BOOKMARK },
          url: {
            requiredIf: b => b.type == TYPE_BOOKMARK,
            validIf: b => b.type == TYPE_BOOKMARK,
          },
          parentGuid: { replaceWith: parentGuid }, 
          title: {
            defaultValue: "",
            validIf: b =>
              b.type == TYPE_BOOKMARK ||
              b.type == TYPE_FOLDER ||
              b.title === "",
          },
          dateAdded: {
            defaultValue: time,
            validIf: b => !b.lastModified || b.dateAdded <= b.lastModified,
          },
          lastModified: {
            defaultValue: time,
            validIf: b =>
              (!b.dateAdded && b.lastModified >= time) ||
              (b.dateAdded && b.lastModified >= b.dateAdded),
          },
          index: { replaceWith: indexToUse++ },
          source: { replaceWith: source },
          keyword: { validIf: b => b.type == TYPE_BOOKMARK },
          charset: { validIf: b => b.type == TYPE_BOOKMARK },
          postData: { validIf: b => b.type == TYPE_BOOKMARK },
          tags: { validIf: b => b.type == TYPE_BOOKMARK },
          children: {
            validIf: b => b.type == TYPE_FOLDER && Array.isArray(b.children),
          },
        };
        if (fixupOrSkipInvalidEntries) {
          validationSchema.guid.fixup = b =>
            (b.guid = lazy.PlacesUtils.history.makeGuid());
          validationSchema.dateAdded.fixup =
            validationSchema.lastModified.fixup = b =>
              (b.lastModified = b.dateAdded = fallbackLastAdded);
        }
        let insertInfo = {};
        try {
          insertInfo = validateBookmarkObject(
            "Bookmarks.sys.mjs: insertTree",
            info,
            validationSchema
          );
        } catch (ex) {
          if (fixupOrSkipInvalidEntries) {
            indexToUse--;
            continue;
          } else {
            throw ex;
          }
        }

        if (shouldUseNullIndices) {
          insertInfo.index = null;
        }
        if (insertInfo.type == Bookmarks.TYPE_BOOKMARK) {
          urlsThatMightNeedPlaces.push(insertInfo.url);
        }

        insertInfos.push(insertInfo);
        if (info.children) {
          let childrenLastAdded = appendInsertionInfoForInfoArray(
            info.children,
            0,
            insertInfo.guid
          );
          if (childrenLastAdded > insertInfo.lastModified) {
            insertInfo.lastModified = childrenLastAdded;
          }
          if (childrenLastAdded > lastAddedForParent) {
            lastAddedForParent = childrenLastAdded;
          }
        }

        if (insertInfo.dateAdded > lastAddedForParent) {
          lastAddedForParent = insertInfo.dateAdded;
        }
      }
      return lastAddedForParent;
    }

    let lastAddedForParent = appendInsertionInfoForInfoArray(
      tree.children,
      null,
      tree.guid
    );

    if (!insertInfos.length) {
      return Promise.resolve([]);
    }

    return (async function () {
      let treeParent = await fetchBookmark({ guid: tree.guid });
      if (!treeParent) {
        throw new Error("The parent you specified doesn't exist.");
      }

      if (treeParent._parentId == lazy.PlacesUtils.tagsFolderId) {
        throw new Error("Can't use insertTree to insert tags.");
      }

      await insertBookmarkTree(
        insertInfos,
        source,
        treeParent,
        urlsThatMightNeedPlaces,
        lastAddedForParent
      );

      let rootIndex = treeParent._childCount;
      for (let insertInfo of insertInfos) {
        if (insertInfo.parentGuid == tree.guid) {
          insertInfo.index += rootIndex++;
        }
      }

      let itemDetailMap = await getBookmarkDetailMap(
        insertInfos.map(info => info.guid)
      );

      let notifications = [];
      for (let i = 0; i < insertInfos.length; i++) {
        let item = insertInfos[i];
        let itemDetail = itemDetailMap.get(item.guid);

        let parentId;
        if (item.parentGuid === treeParent.guid) {
          parentId = treeParent._id;
        } else {
          parentId = itemDetail.parentId;
        }

        let url = "";
        if (item.type == Bookmarks.TYPE_BOOKMARK) {
          url = URL.isInstance(item.url) ? item.url.href : item.url;
        }

        notifications.push(
          new PlacesBookmarkAddition({
            id: itemDetail.id,
            url,
            itemType: item.type,
            parentId,
            index: item.index,
            title: item.title,
            dateAdded: item.dateAdded,
            guid: item.guid,
            parentGuid: item.parentGuid,
            source: item.source,
            isTagging: false,
            tags: itemDetail.tags,
            frecency: itemDetail.frecency,
            hidden: itemDetail.hidden,
            visitCount: itemDetail.visitCount,
            lastVisitDate: itemDetail.lastVisitDate,
            targetFolderGuid: itemDetail.targetFolderGuid,
            targetFolderItemId: itemDetail.targetFolderItemId,
            targetFolderTitle: itemDetail.targetFolderTitle,
          })
        );

        try {
          await handleBookmarkItemSpecialData(item);
        } catch (ex) {
          console.error(
            "An error occured while handling special bookmark data:",
            ex
          );
        }

        delete item.source;

        insertInfos[i] = Object.assign({}, item);
      }

      if (notifications.length) {
        PlacesObservers.notifyListeners(notifications);
      }

      return insertInfos;
    })();
  },

  update(info) {
    let updateInfo = validateBookmarkObject("Bookmarks.sys.mjs: update", info, {
      guid: { required: true },
      index: {
        requiredIf: b => b.hasOwnProperty("parentGuid"),
        validIf: b => b.index >= 0 || b.index == this.DEFAULT_INDEX,
      },
      parentGuid: { validIf: b => b.parentGuid != this.rootGuid },
      source: { defaultValue: this.SOURCES.DEFAULT },
    });

    if (Object.keys(updateInfo).length < 3) {
      throw new Error("Not enough properties to update");
    }

    return (async () => {
      let item = await fetchBookmark(updateInfo);
      if (!item) {
        throw new Error("No bookmarks found for the provided GUID");
      }
      if (updateInfo.hasOwnProperty("type") && updateInfo.type != item.type) {
        throw new Error("The bookmark type cannot be changed");
      }

      removeSameValueProperties(updateInfo, item);
      if (Object.keys(updateInfo).length < 3) {
        return Object.assign({}, item);
      }
      const now = new Date();
      let lastModifiedDefault = now;
      if (!("lastModified" in updateInfo) && "dateAdded" in updateInfo) {
        lastModifiedDefault = new Date(
          Math.max(item.lastModified, updateInfo.dateAdded)
        );
      }
      updateInfo = validateBookmarkObject(
        "Bookmarks.sys.mjs: update",
        updateInfo,
        {
          url: { validIf: () => item.type == this.TYPE_BOOKMARK },
          title: {
            validIf: () =>
              [this.TYPE_BOOKMARK, this.TYPE_FOLDER].includes(item.type),
          },
          lastModified: {
            defaultValue: lastModifiedDefault,
            validIf: b =>
              b.lastModified >= now ||
              b.lastModified >= (b.dateAdded || item.dateAdded),
          },
          dateAdded: { defaultValue: item.dateAdded },
        }
      );

      return lazy.PlacesUtils.withConnectionWrapper(
        "Bookmarks.sys.mjs: update",
        async db => {
          let parent;
          if (updateInfo.hasOwnProperty("parentGuid")) {
            if (lazy.PlacesUtils.isRootItem(item.guid)) {
              throw new Error("It's not possible to move Places root folders.");
            }
            if (item.type == this.TYPE_FOLDER) {
              let rows = await db.executeCached(
                `WITH RECURSIVE
               descendants(did) AS (
                 VALUES(:id)
                 UNION ALL
                 SELECT id FROM moz_bookmarks
                 JOIN descendants ON parent = did
                 WHERE type = :type
               )
               SELECT guid FROM moz_bookmarks
               WHERE id IN descendants
              `,
                { id: item._id, type: this.TYPE_FOLDER }
              );
              if (
                rows
                  .map(r => r.getResultByName("guid"))
                  .includes(updateInfo.parentGuid)
              ) {
                throw new Error(
                  "Cannot insert a folder into itself or one of its descendants"
                );
              }
            }

            parent = await fetchBookmark({ guid: updateInfo.parentGuid });
            if (!parent) {
              throw new Error("No bookmarks found for the provided parentGuid");
            }
          }

          if (updateInfo.hasOwnProperty("index")) {
            if (lazy.PlacesUtils.isRootItem(item.guid)) {
              throw new Error("It's not possible to move Places root folders.");
            }
            if (!parent) {
              parent = await fetchBookmark({ guid: item.parentGuid });
            }

            if (
              updateInfo.index >= parent._childCount ||
              updateInfo.index == this.DEFAULT_INDEX
            ) {
              updateInfo.index = parent._childCount;

              if (parent.guid == item.parentGuid) {
                updateInfo.index--;
              }
            }
          }

          let updatedItem = await db.executeTransaction(async function () {
            let innerUpdatedItem = await updateBookmark(
              db,
              updateInfo,
              item,
              item.index,
              parent
            );
            if (parent) {
              await setAncestorsLastModified(
                db,
                parent.guid,
                innerUpdatedItem.lastModified
              );
            }
            return innerUpdatedItem;
          });

          const notifications = [];

          if (
            (info.hasOwnProperty("lastModified") &&
              updateInfo.hasOwnProperty("lastModified") &&
              item.lastModified != updatedItem.lastModified) ||
            (info.hasOwnProperty("dateAdded") &&
              updateInfo.hasOwnProperty("dateAdded") &&
              item.dateAdded != updatedItem.dateAdded)
          ) {
            let isTagging = updatedItem.parentGuid == Bookmarks.tagsGuid;
            if (!isTagging) {
              if (!parent) {
                parent = await fetchBookmark({ guid: updatedItem.parentGuid });
              }
              isTagging = parent.parentGuid === Bookmarks.tagsGuid;
            }

            notifications.push(
              new PlacesBookmarkTime({
                id: updatedItem._id,
                itemType: updatedItem.type,
                url: updatedItem.url?.href,
                guid: updatedItem.guid,
                parentGuid: updatedItem.parentGuid,
                dateAdded: updatedItem.dateAdded,
                lastModified: updatedItem.lastModified,
                source: updatedItem.source,
                isTagging,
              })
            );
          }

          if (updateInfo.hasOwnProperty("title")) {
            let isTagging = updatedItem.parentGuid == Bookmarks.tagsGuid;
            if (!isTagging) {
              if (!parent) {
                parent = await fetchBookmark({ guid: updatedItem.parentGuid });
              }
              isTagging = parent.parentGuid === Bookmarks.tagsGuid;
            }

            notifications.push(
              new PlacesBookmarkTitle({
                id: updatedItem._id,
                itemType: updatedItem.type,
                url: updatedItem.url?.href,
                guid: updatedItem.guid,
                parentGuid: updatedItem.parentGuid,
                title: updatedItem.title,
                lastModified: updatedItem.lastModified,
                source: updatedItem.source,
                isTagging,
              })
            );

            if (isTagging) {
              for (let entry of await fetchBookmarksByTags(
                { tags: [updatedItem.title] },
                { concurrent: true }
              )) {
                notifications.push(
                  new PlacesBookmarkTags({
                    id: entry._id,
                    itemType: entry.type,
                    url: entry.url,
                    guid: entry.guid,
                    parentGuid: entry.parentGuid,
                    tags: entry._tags,
                    lastModified: entry.lastModified,
                    source: updatedItem.source,
                    isTagging: false,
                  })
                );
              }
            }
          }
          if (updateInfo.hasOwnProperty("url")) {
            await lazy.PlacesUtils.keywords.reassign(
              item.url,
              updatedItem.url,
              updatedItem.source
            );

            let isTagging = updatedItem.parentGuid == Bookmarks.tagsGuid;
            if (!isTagging) {
              if (!parent) {
                parent = await fetchBookmark({ guid: updatedItem.parentGuid });
              }
              isTagging = parent.parentGuid === Bookmarks.tagsGuid;
            }

            notifications.push(
              new PlacesBookmarkUrl({
                id: updatedItem._id,
                itemType: updatedItem.type,
                url: updatedItem.url.href,
                guid: updatedItem.guid,
                parentGuid: updatedItem.parentGuid,
                source: updatedItem.source,
                isTagging,
                lastModified: updatedItem.lastModified,
              })
            );
          }
          if (
            item.parentGuid != updatedItem.parentGuid ||
            item.index != updatedItem.index
          ) {
            let details = (await getBookmarkDetailMap([updatedItem.guid])).get(
              updatedItem.guid
            );
            notifications.push(
              new PlacesBookmarkMoved({
                id: updatedItem._id,
                itemType: updatedItem.type,
                url: updatedItem.url && updatedItem.url.href,
                guid: updatedItem.guid,
                parentGuid: updatedItem.parentGuid,
                source: updatedItem.source,
                index: updatedItem.index,
                oldParentGuid: item.parentGuid,
                oldIndex: item.index,
                isTagging:
                  updatedItem.parentGuid === Bookmarks.tagsGuid ||
                  parent.parentGuid === Bookmarks.tagsGuid,
                title: updatedItem.title,
                tags: details.tags,
                frecency: details.frecency,
                hidden: details.hidden,
                visitCount: details.visitCount,
                dateAdded: updatedItem.dateAdded ?? Date.now(),
                lastVisitDate: details.lastVisitDate,
              })
            );
          }

          if (notifications.length) {
            PlacesObservers.notifyListeners(notifications);
          }

          delete updatedItem.source;
          return Object.assign({}, updatedItem);
        }
      );
    })();
  },

  moveToFolder(guids, parentGuid, index, source) {
    if (!Array.isArray(guids) || guids.length < 1) {
      throw new Error("guids should be an array of at least one item");
    }
    if (!guids.every(guid => lazy.PlacesUtils.isValidGuid(guid))) {
      throw new Error("Expected only valid GUIDs to be passed.");
    }
    if (parentGuid && !lazy.PlacesUtils.isValidGuid(parentGuid)) {
      throw new Error("parentGuid should be a valid GUID");
    }
    if (parentGuid == lazy.PlacesUtils.bookmarks.rootGuid) {
      throw new Error("Cannot move bookmarks into root.");
    }
    if (typeof index != "number" || index < this.DEFAULT_INDEX) {
      throw new Error(
        `index should be a number greater than ${this.DEFAULT_INDEX}`
      );
    }

    if (!source) {
      source = this.SOURCES.DEFAULT;
    }

    return (async () => {
      let updateInfos = [];
      await lazy.PlacesUtils.withConnectionWrapper(
        "Bookmarks.sys.mjs: moveToFolder",
        async db => {
          const lastModified = new Date();

          let targetParentGuid = parentGuid || undefined;

          for (let guid of guids) {
            let existingItem = await fetchBookmark({ guid }, { db });
            if (!existingItem) {
              throw new Error("No bookmarks found for the provided GUID");
            }

            if (parentGuid) {
              if (existingItem.type == this.TYPE_FOLDER) {
                let rows = await db.executeCached(
                  `WITH RECURSIVE
                 descendants(did) AS (
                   VALUES(:id)
                   UNION ALL
                   SELECT id FROM moz_bookmarks
                   JOIN descendants ON parent = did
                   WHERE type = :type
                 )
                 SELECT guid FROM moz_bookmarks
                 WHERE id IN descendants
                `,
                  { id: existingItem._id, type: this.TYPE_FOLDER }
                );
                if (
                  rows.map(r => r.getResultByName("guid")).includes(parentGuid)
                ) {
                  throw new Error(
                    "Cannot insert a folder into itself or one of its descendants"
                  );
                }
              }
            } else if (!targetParentGuid) {
              targetParentGuid = existingItem.parentGuid;
            } else if (existingItem.parentGuid != targetParentGuid) {
              throw new Error(
                "All bookmarks should be in the same folder if no parent is specified"
              );
            }

            updateInfos.push({
              existingItem,
              currIndex: existingItem.index,
              updatedItem: null,
              newParent: null,
            });
          }

          let newParent = await fetchBookmark(
            { guid: targetParentGuid },
            { db }
          );

          if (newParent._grandParentId == lazy.PlacesUtils.tagsFolderId) {
            throw new Error("Can't move to a tags folder");
          }

          let newParentChildCount = newParent._childCount;

          await db.executeTransaction(async () => {
            for (let i = 0; i < updateInfos.length; i++) {
              let info = updateInfos[i];
              if (index != this.DEFAULT_INDEX) {
                if (info.existingItem.parentGuid == newParent.guid) {
                  if (index > info.existingItem.index) {
                    index--;
                  } else if (index == info.existingItem.index) {
                    info.updatedItem = { ...info.existingItem };
                    continue;
                  }
                }
              }

              if (index == this.DEFAULT_INDEX || index >= newParentChildCount) {
                index = newParentChildCount;

                if (info.existingItem.parentGuid == newParent.guid) {
                  index--;
                }
              }

              info.updatedItem = await updateBookmark(
                db,
                { lastModified, index },
                info.existingItem,
                info.currIndex,
                newParent
              );
              info.newParent = newParent;

              if (info.existingItem.parentGuid == newParent.guid) {
                let sign = index < info.currIndex ? 1 : -1;
                for (let j = 0; j < updateInfos.length; j++) {
                  if (j == i) {
                    continue;
                  }
                  if (
                    updateInfos[j].currIndex >=
                      Math.min(info.currIndex, index) &&
                    updateInfos[j].currIndex <= Math.max(info.currIndex, index)
                  ) {
                    updateInfos[j].currIndex += sign;
                  }
                }
              }
              info.currIndex = index;

              if (info.existingItem.parentGuid != newParent.guid) {
                newParentChildCount++;
              }
              index++;
            }

            await setAncestorsLastModified(
              db,
              newParent.guid,
              lastModified
            );
          });
        }
      );

      const notifications = [];
      let detailsMap = await getBookmarkDetailMap(
        updateInfos.map(({ updatedItem }) => updatedItem.guid)
      );
      for (let { updatedItem, existingItem, newParent } of updateInfos) {
        if (
          existingItem.parentGuid != updatedItem.parentGuid ||
          existingItem.index != updatedItem.index
        ) {
          let details = detailsMap.get(updatedItem.guid);
          notifications.push(
            new PlacesBookmarkMoved({
              id: updatedItem._id,
              itemType: updatedItem.type,
              url: existingItem.url,
              guid: updatedItem.guid,
              parentGuid: updatedItem.parentGuid,
              source,
              index: updatedItem.index,
              oldParentGuid: existingItem.parentGuid,
              oldIndex: existingItem.index,
              isTagging:
                updatedItem.parentGuid === Bookmarks.tagsGuid ||
                newParent.parentGuid === Bookmarks.tagsGuid,
              title: updatedItem.title,
              tags: details.tags,
              frecency: details.frecency,
              hidden: details.hidden,
              visitCount: details.visitCount,
              dateAdded: updatedItem.dateAdded,
              lastVisitDate: details.lastVisitDate,
            })
          );
        }
        delete updatedItem.source;
      }

      if (notifications.length) {
        PlacesObservers.notifyListeners(notifications);
      }

      return updateInfos.map(updateInfo =>
        Object.assign({}, updateInfo.updatedItem)
      );
    })();
  },

  remove(guidOrInfo, options = {}) {
    let infos = guidOrInfo;
    if (!infos) {
      throw new Error("Input should be a valid object");
    }
    if (!Array.isArray(guidOrInfo)) {
      if (typeof guidOrInfo != "object") {
        infos = [{ guid: guidOrInfo }];
      } else {
        infos = [guidOrInfo];
      }
    }

    if (!("source" in options)) {
      options.source = Bookmarks.SOURCES.DEFAULT;
    }

    let removeInfos = [];
    for (let info of infos) {
      if (
        [
          Bookmarks.rootGuid,
          Bookmarks.menuGuid,
          Bookmarks.toolbarGuid,
          Bookmarks.unfiledGuid,
          Bookmarks.tagsGuid,
          Bookmarks.mobileGuid,
        ].includes(info.guid)
      ) {
        throw new Error("It's not possible to remove Places root folders.");
      }

      let removeInfo = validateBookmarkObject(
        "Bookmarks.sys.mjs: remove",
        info
      );
      removeInfos.push(removeInfo);
    }

    return (async function () {
      let removeItems = [];
      for (let info of removeInfos) {
        let item = await fetchBookmark(info, { ignoreInvalidURLs: true });
        if (!item) {
          throw new Error("No bookmarks found for the provided GUID.");
        }

        removeItems.push(item);
      }

      await removeBookmarks(removeItems, options);

      let notifications = [];

      for (let item of removeItems) {
        let isUntagging = item._grandParentId == lazy.PlacesUtils.tagsFolderId;
        let url = "";
        if (item.type == Bookmarks.TYPE_BOOKMARK) {
          url = item.hasOwnProperty("url") ? item.url.href : null;
        }

        notifications.push(
          new PlacesBookmarkRemoved({
            id: item._id,
            url,
            title: item.title,
            itemType: item.type,
            parentId: item._parentId,
            index: item.index,
            guid: item.guid,
            parentGuid: item.parentGuid,
            source: options.source,
            isTagging: isUntagging,
            isDescendantRemoval: false,
          })
        );

        if (isUntagging) {
          for (let entry of await fetchBookmarksByURL(item, {
            concurrent: true,
          })) {
            notifications.push(
              new PlacesBookmarkTags({
                id: entry._id,
                itemType: entry.type,
                url,
                guid: entry.guid,
                parentGuid: entry.parentGuid,
                tags: entry._tags,
                lastModified: entry.lastModified,
                source: options.source,
                isTagging: false,
              })
            );
          }
        }
      }

      PlacesObservers.notifyListeners(notifications);
    })();
  },

  eraseEverything(options = {}) {
    if (!options.source) {
      options.source = Bookmarks.SOURCES.DEFAULT;
    }

    return lazy.PlacesUtils.withConnectionWrapper(
      "Bookmarks.sys.mjs: eraseEverything",
      async function (db) {
        let urls = [];
        await db.executeTransaction(async function () {
          urls = await removeFoldersContents(
            db,
            Bookmarks.userContentRoots,
            options
          );
          const time = lazy.PlacesUtils.toPRTime(new Date());
          for (let folderGuid of Bookmarks.userContentRoots) {
            await db.executeCached(
              `UPDATE moz_bookmarks SET lastModified = :time
               WHERE id IN (SELECT id FROM moz_bookmarks WHERE guid = :folderGuid )
              `,
              { folderGuid, time }
            );
          }
        });

        if (urls?.length) {
          await lazy.PlacesUtils.keywords.eraseEverything();
        }
      }
    );
  },

  getRecent(numberOfItems) {
    if (numberOfItems === undefined) {
      throw new Error("numberOfItems argument is required");
    }
    if (typeof numberOfItems !== "number" || numberOfItems % 1 !== 0) {
      throw new Error("numberOfItems argument must be an integer");
    }
    if (numberOfItems <= 0) {
      throw new Error("numberOfItems argument must be greater than zero");
    }

    return fetchRecentBookmarks(numberOfItems);
  },

  async fetch(guidOrInfo, onResult = null, options = {}) {
    if (onResult && typeof onResult != "function") {
      throw new Error("onResult callback must be a valid function");
    }
    let info = guidOrInfo;
    if (!info) {
      throw new Error("Input should be a valid object");
    }
    if (typeof info != "object") {
      info = { guid: guidOrInfo };
    } else if (Object.keys(info).length == 1) {
      if (
        !["url", "guid", "parentGuid", "index", "guidPrefix", "tags"].includes(
          Object.keys(info)[0]
        )
      ) {
        throw new Error(`Unexpected number of conditions provided: 0`);
      }
    } else {
      let conditionsCount = [
        v => v.hasOwnProperty("guid"),
        v => v.hasOwnProperty("parentGuid") && v.hasOwnProperty("index"),
        v => v.hasOwnProperty("url"),
        v => v.hasOwnProperty("guidPrefix"),
        v => v.hasOwnProperty("tags"),
      ].reduce((old, fn) => (old + fn(info)) | 0, 0);
      if (conditionsCount != 1) {
        throw new Error(
          `Unexpected number of conditions provided: ${conditionsCount}`
        );
      }
    }

    options = {
      concurrent: !!options.concurrent,
      includePath: !!options.includePath,
      includeItemIds: !!options.includeItemIds,
    };

    let behavior = {};
    if (info.hasOwnProperty("parentGuid") || info.hasOwnProperty("index")) {
      behavior = {
        parentGuid: { requiredIf: b => b.hasOwnProperty("index") },
        index: {
          validIf: b =>
            (typeof b.index == "number" && b.index >= 0) ||
            b.index == this.DEFAULT_INDEX,
        },
      };
    }

    let fetchInfo = validateBookmarkObject(
      "Bookmarks.sys.mjs: fetch",
      info,
      behavior
    );

    let results;
    if (fetchInfo.hasOwnProperty("url")) {
      results = await fetchBookmarksByURL(fetchInfo, options);
    } else if (fetchInfo.hasOwnProperty("guid")) {
      results = await fetchBookmark(fetchInfo, options);
    } else if (fetchInfo.hasOwnProperty("parentGuid")) {
      if (fetchInfo.hasOwnProperty("index")) {
        results = await fetchBookmarkByPosition(fetchInfo, options);
      } else {
        results = await fetchBookmarksByParentGUID(fetchInfo, options);
      }
    } else if (fetchInfo.hasOwnProperty("guidPrefix")) {
      results = await fetchBookmarksByGUIDPrefix(fetchInfo, options);
    } else if (fetchInfo.hasOwnProperty("tags")) {
      results = await fetchBookmarksByTags(fetchInfo, options);
    }

    if (!results) {
      return null;
    }

    if (!Array.isArray(results)) {
      results = [results];
    }
    results = results.map(r => {
      if (r.type == this.TYPE_FOLDER) {
        r.childCount = r._childCount;
      }
      if (options.includeItemIds) {
        r.itemId = r._id;
        r.parentId = r._parentId;
      }
      return Object.assign({}, r);
    });

    if (options.includePath && results.length) {
      let parentGuids = [...new Set(results.map(r => r.parentGuid))];
      let pathsByGuid = await retrieveFullBookmarkPaths(parentGuids);
      for (let result of results) {
        result.path = pathsByGuid.get(result.parentGuid) ?? [];
      }
    }

    if (onResult) {
      for (let result of results) {
        try {
          onResult(result);
        } catch (ex) {
          console.error(ex);
        }
      }
    }

    return results[0];
  },
  /* eslint-disable-next-line jsdoc/require-returns-check */
  fetchTree(guid, options) {
    throw new Error(`Not yet implemented ${guid} ${options}`);
  },

  async fetchTags() {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    let rows = await db.executeCached(
      `
      SELECT b.title AS name, count(*) AS count
      FROM moz_bookmarks b
      JOIN moz_bookmarks p ON b.parent = p.id
      JOIN moz_bookmarks c ON c.parent = b.id
      WHERE p.guid = :tagsGuid
      GROUP BY name
      ORDER BY name COLLATE nocase ASC
    `,
      { tagsGuid: this.tagsGuid }
    );
    return rows.map(r => ({
      name: r.getResultByName("name"),
      count: r.getResultByName("count"),
    }));
  },

  reorder(parentGuid, orderedChildrenGuids, options = {}) {
    let info = { guid: parentGuid };
    info = validateBookmarkObject("Bookmarks.sys.mjs: reorder", info, {
      guid: { required: true },
    });

    if (!Array.isArray(orderedChildrenGuids) || !orderedChildrenGuids.length) {
      throw new Error("Must provide a sorted array of children GUIDs.");
    }
    try {
      orderedChildrenGuids.forEach(lazy.PlacesUtils.BOOKMARK_VALIDATORS.guid);
    } catch (ex) {
      throw new Error("Invalid GUID found in the sorted children array.");
    }

    options.source =
      "source" in options
        ? lazy.PlacesUtils.BOOKMARK_VALIDATORS.source(options.source)
        : Bookmarks.SOURCES.DEFAULT;
    options.lastModified =
      "lastModified" in options
        ? lazy.PlacesUtils.BOOKMARK_VALIDATORS.lastModified(
            options.lastModified
          )
        : new Date();

    return (async () => {
      let parent = await fetchBookmark(info);
      if (!parent || parent.type != this.TYPE_FOLDER) {
        throw new Error("No folder found for the provided GUID.");
      }
      if (parent._childCount == 0) {
        return;
      }

      let sortedChildren = await reorderChildren(
        parent,
        orderedChildrenGuids,
        options
      );

      const notifications = [];
      let detailsMap = await getBookmarkDetailMap(
        sortedChildren.map(c => c.guid)
      );
      for (let child of sortedChildren) {
        let details = detailsMap.get(child.guid);
        notifications.push(
          new PlacesBookmarkMoved({
            id: child._id,
            itemType: child.type,
            url: child.url?.href,
            guid: child.guid,
            parentGuid: child.parentGuid,
            source: options.source,
            index: child.index,
            oldParentGuid: child.parentGuid,
            oldIndex: child._oldIndex,
            isTagging:
              child.parentGuid === Bookmarks.tagsGuid ||
              parent.parentGuid === Bookmarks.tagsGuid,
            title: child.title,
            tags: details.tags,
            frecency: details.frecency,
            hidden: details.hidden,
            visitCount: details.visitCount,
            dateAdded: child.dateAdded,
            lastVisitDate: details.lastVisitDate,
          })
        );
      }
      if (notifications.length) {
        PlacesObservers.notifyListeners(notifications);
      }
    })();
  },

  search(query) {
    if (!query) {
      throw new Error("Query object is required");
    }
    if (typeof query === "string") {
      query = { query };
    }
    if (typeof query !== "object") {
      throw new Error("Query must be an object or a string");
    }
    if (query.query && typeof query.query !== "string") {
      throw new Error("Query option must be a string");
    }
    if (query.title && typeof query.title !== "string") {
      throw new Error("Title option must be a string");
    }

    if (query.url) {
      if (typeof query.url === "string") {
        query.url = new URL(query.url).href;
      } else if (URL.isInstance(query.url)) {
        query.url = query.url.href;
      } else if (query.url instanceof Ci.nsIURI) {
        query.url = query.url.spec;
      } else {
        throw new Error("Url option must be a string or a URL object");
      }
    }

    return queryBookmarks(query);
  },
});



async function updateBookmark(db, info, item, oldIndex, newParent) {
  let tuples = new Map();
  tuples.set("lastModified", {
    value: lazy.PlacesUtils.toPRTime(info.lastModified),
  });
  if (info.hasOwnProperty("title")) {
    tuples.set("title", {
      value: info.title,
      fragment: `title = NULLIF(:title, '')`,
    });
  }
  if (info.hasOwnProperty("dateAdded")) {
    tuples.set("dateAdded", {
      value: lazy.PlacesUtils.toPRTime(info.dateAdded),
    });
  }

  if (info.hasOwnProperty("url")) {
    await lazy.PlacesUtils.maybeInsertPlace(db, info.url);
    tuples.set("url", {
      value: info.url.href,
      fragment:
        "fk = (SELECT id FROM moz_places WHERE url_hash = hash(:url) AND url = :url)",
    });
  }

  let newIndex = info.hasOwnProperty("index") ? info.index : item.index;
  if (newParent) {
    tuples.set("position", { value: newIndex });

    if (newParent.guid == item.parentGuid) {
      await db.executeCached(
        `UPDATE moz_bookmarks
           SET position = CASE WHEN :newIndex < :currIndex
             THEN position + 1
             ELSE position - 1
           END
           WHERE parent = :newParentId
             AND position BETWEEN :lowIndex AND :highIndex
          `,
        {
          newIndex,
          currIndex: oldIndex,
          newParentId: newParent._id,
          lowIndex: Math.min(oldIndex, newIndex),
          highIndex: Math.max(oldIndex, newIndex),
        }
      );
    } else {
      tuples.set("parent", { value: newParent._id });
      await db.executeCached(
        `UPDATE moz_bookmarks SET position = position - 1
         WHERE parent = :oldParentId
           AND position >= :oldIndex
        `,
        { oldParentId: item._parentId, oldIndex }
      );
      await db.executeCached(
        `UPDATE moz_bookmarks SET position = position + 1
         WHERE parent = :newParentId
           AND position >= :newIndex
        `,
        { newParentId: newParent._id, newIndex }
      );

      await setAncestorsLastModified(
        db,
        item.parentGuid,
        info.lastModified
      );
    }
  }

  await db.executeCached(
    `UPDATE moz_bookmarks
     SET ${Array.from(tuples.keys())
       .map(v => tuples.get(v).fragment || `${v} = :${v}`)
       .join(", ")}
     WHERE guid = :guid
    `,
    Object.assign(
      { guid: item.guid },
      [...tuples.entries()].reduce((p, c) => {
        p[c[0]] = c[1].value;
        return p;
      }, {})
    )
  );

  let additionalParentInfo = {};
  if (newParent) {
    additionalParentInfo.parentGuid = newParent.guid;
    Object.defineProperty(additionalParentInfo, "_parentId", {
      value: newParent._id,
      enumerable: false,
    });
    Object.defineProperty(additionalParentInfo, "_grandParentId", {
      value: newParent._parentId,
      enumerable: false,
    });
  }

  return mergeIntoNewObject(item, info, additionalParentInfo);
}


function insertBookmark(item, parent) {
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: insertBookmark",
    async function (db) {
      if (!item.hasOwnProperty("guid")) {
        item.guid = lazy.PlacesUtils.history.makeGuid();
      }

      await db.executeTransaction(async function transaction() {
        if (item.type == Bookmarks.TYPE_BOOKMARK) {
          await lazy.PlacesUtils.maybeInsertPlace(db, item.url);
        }

        await db.executeCached(
          `UPDATE moz_bookmarks SET position = position + 1
         WHERE parent = :parent
         AND position >= :index
        `,
          { parent: parent._id, index: item.index }
        );

        await db.executeCached(
          `INSERT INTO moz_bookmarks (fk, type, parent, position, title,
                                    dateAdded, lastModified, guid)
         VALUES (CASE WHEN :url ISNULL THEN NULL ELSE (SELECT id FROM moz_places WHERE url_hash = hash(:url) AND url = :url) END,
                 :type, :parent, :index, NULLIF(:title, ''), :date_added,
                 :last_modified, :guid)
        `,
          {
            url: item.hasOwnProperty("url") ? item.url.href : null,
            type: item.type,
            parent: parent._id,
            index: item.index,
            title: item.title,
            date_added: lazy.PlacesUtils.toPRTime(item.dateAdded),
            last_modified: lazy.PlacesUtils.toPRTime(item.lastModified),
            guid: item.guid,
          }
        );

        await setAncestorsLastModified(
          db,
          item.parentGuid,
          item.dateAdded
        );
      });

      return item;
    }
  );
}

function insertBookmarkTree(items, source, parent, urls, lastAddedForParent) {
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: insertBookmarkTree",
    async function (db) {
      await db.executeTransaction(async function transaction() {
        await lazy.PlacesUtils.maybeInsertManyPlaces(db, urls);

        let rootId = parent._id;

        items = items.map(item => ({
          url: item.url && item.url.href,
          type: item.type,
          parentGuid: item.parentGuid,
          index: item.index,
          title: item.title,
          date_added: lazy.PlacesUtils.toPRTime(item.dateAdded),
          last_modified: lazy.PlacesUtils.toPRTime(item.lastModified),
          guid: item.guid,
          rootId,
        }));
        await db.executeCached(
          `INSERT INTO moz_bookmarks (fk, type, parent, position, title,
                                    dateAdded, lastModified, guid)
         VALUES (CASE WHEN :url ISNULL THEN NULL ELSE (SELECT id FROM moz_places WHERE url_hash = hash(:url) AND url = :url) END, :type,
         (SELECT id FROM moz_bookmarks WHERE guid = :parentGuid),
         IFNULL(:index, (SELECT count(*) FROM moz_bookmarks WHERE parent = :rootId)),
         NULLIF(:title, ''), :date_added, :last_modified, :guid)`,
          items
        );

        await setAncestorsLastModified(
          db,
          parent.guid,
          lastAddedForParent
        );
      });

      return items;
    }
  );
}

async function handleBookmarkItemSpecialData(item) {
  if ("keyword" in item && item.keyword) {
    try {
      await lazy.PlacesUtils.keywords.insert({
        keyword: item.keyword,
        url: item.url,
        postData: item.postData,
        source: item.source,
      });
    } catch (ex) {
      console.error(
        `Failed to insert keyword "${item.keyword} for ${item.url}":`,
        ex
      );
    }
  }
  if ("tags" in item) {
    try {
      lazy.PlacesUtils.tagging.tagURI(
        Services.io.newURI(item.url),
        item.tags,
        item.source
      );
    } catch (ex) {
      console.error(
        `Unable to set tags "${item.tags.join(", ")}" for ${item.url}:`,
        ex
      );
    }
  }
  if ("charset" in item && item.charset) {
    try {
      let charset = item.charset;
      if (item.charset.toLowerCase() == "utf-8") {
        charset = null;
      }

      await lazy.PlacesUtils.history.update({
        url: item.url,
        annotations: new Map([[lazy.PlacesUtils.CHARSET_ANNO, charset]]),
      });
    } catch (ex) {
      console.error(
        `Failed to set charset "${item.charset}" for ${item.url}:`,
        ex
      );
    }
  }
}


async function queryBookmarks(info) {
  let queryParams = {
    tags_folder: lazy.PlacesUtils.tagsFolderId,
  };
  let queryString = "WHERE b.parent <> :tags_folder";
  queryString += " AND p.parent <> :tags_folder";

  if (info.title) {
    queryString += " AND b.title = :title";
    queryParams.title = info.title;
  }

  if (info.url) {
    queryString += " AND h.url_hash = hash(:url) AND h.url = :url";
    queryParams.url = info.url;
  }

  if (info.query) {
    queryString +=
      " AND AUTOCOMPLETE_MATCH(:query, h.url, b.title, NULL, NULL, 1, 1, NULL, :matchBehavior, :searchBehavior, NULL) ";
    queryParams.query = info.query;
    queryParams.matchBehavior = MATCH_ANYWHERE_UNMODIFIED;
    queryParams.searchBehavior = BEHAVIOR_BOOKMARK;
  }

  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: queryBookmarks",
    async function (db) {
      let rows = await db.executeCached(
        `SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
                b.dateAdded, b.lastModified, b.type,
                IFNULL(b.title, '') AS title, h.url AS url, b.parent, p.parent,
                NULL AS _id,
                NULL AS _childCount,
                NULL AS _grandParentId,
                NULL AS _parentId
         FROM moz_bookmarks b
         LEFT JOIN moz_bookmarks p ON p.id = b.parent
         LEFT JOIN moz_places h ON h.id = b.fk
         ${queryString}
        `,
        queryParams
      );

      return rowsToItemsArray(rows);
    }
  );
}

async function fetchBookmark(info, options = {}) {
  let query = async function (db) {
    let rows = await db.executeCached(
      `SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
              b.dateAdded, b.lastModified, b.type, IFNULL(b.title, '') AS title,
              h.url AS url, b.id AS _id, b.parent AS _parentId,
              (SELECT count(*) FROM moz_bookmarks WHERE parent = b.id) AS _childCount,
              p.parent AS _grandParentId
       FROM moz_bookmarks b
       LEFT JOIN moz_bookmarks p ON p.id = b.parent
       LEFT JOIN moz_places h ON h.id = b.fk
       WHERE b.guid = :guid
      `,
      { guid: info.guid }
    );

    return rows.length
      ? rowsToItemsArray(rows, !!options.ignoreInvalidURLs)[0]
      : null;
  };
  if (options.concurrent) {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    return query(db);
  }
  if (options.db) {
    return query(options.db);
  }
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: fetchBookmark",
    query
  );
}

async function fetchBookmarkByPosition(info, options = {}) {
  let query = async function (db) {
    let index = info.index == Bookmarks.DEFAULT_INDEX ? null : info.index;
    let rows = await db.executeCached(
      `SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
              b.dateAdded, b.lastModified, b.type, IFNULL(b.title, '') AS title,
              h.url AS url, b.id AS _id, b.parent AS _parentId,
              (SELECT count(*) FROM moz_bookmarks WHERE parent = b.id) AS _childCount,
              p.parent AS _grandParentId
       FROM moz_bookmarks b
       LEFT JOIN moz_bookmarks p ON p.id = b.parent
       LEFT JOIN moz_places h ON h.id = b.fk
       WHERE p.guid = :parentGuid
       AND b.position = IFNULL(:index, (SELECT count(*) - 1
                                        FROM moz_bookmarks
                                        WHERE parent = p.id))
      `,
      { parentGuid: info.parentGuid, index }
    );

    return rows.length ? rowsToItemsArray(rows)[0] : null;
  };
  if (options.concurrent) {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    return query(db);
  }
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: fetchBookmarkByPosition",
    query
  );
}

async function fetchBookmarksByTags(info, options = {}) {
  let query = async function (db) {
    let rows = await db.executeCached(
      `SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
              b.dateAdded, b.lastModified, b.type, IFNULL(b.title, '') AS title,
              h.url AS url, b.id AS _id, b.parent AS _parentId,
              NULL AS _childCount,
              p.parent AS _grandParentId,
              (SELECT group_concat(pp.title ORDER BY pp.title)
               FROM moz_bookmarks bb
               JOIN moz_bookmarks pp ON pp.id = bb.parent
               JOIN moz_bookmarks gg ON gg.id = pp.parent
               WHERE bb.fk = h.id
               AND gg.guid = '${Bookmarks.tagsGuid}'
              ) AS _tags
       FROM moz_bookmarks b
       JOIN moz_bookmarks p ON p.id = b.parent
       JOIN moz_bookmarks g ON g.id = p.parent
       JOIN moz_places h ON h.id = b.fk
       WHERE g.guid <> '${Bookmarks.tagsGuid}'
       AND b.fk IN (
          SELECT b2.fk FROM moz_bookmarks b2
          JOIN moz_bookmarks p2 ON p2.id = b2.parent
          JOIN moz_bookmarks g2 ON g2.id = p2.parent
          WHERE g2.guid = '${Bookmarks.tagsGuid}'
          AND lower(p2.title) IN (
            ${new Array(info.tags.length).fill("?").join(",")}
          )
          GROUP BY b2.fk HAVING count(*) = ${info.tags.length}
       )
       ORDER BY b.lastModified DESC
      `,
      info.tags.map(t => t.toLowerCase())
    );

    return rows.length ? rowsToItemsArray(rows) : null;
  };

  if (options.concurrent) {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    return query(db);
  }
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: fetchBookmarksByTags",
    query
  );
}

async function fetchBookmarksByGUIDPrefix(info, options = {}) {
  let query = async function (db) {
    let rows = await db.executeCached(
      `SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
              b.dateAdded, b.lastModified, b.type, IFNULL(b.title, '') AS title,
              h.url AS url, b.id AS _id, b.parent AS _parentId,
              (SELECT count(*) FROM moz_bookmarks WHERE parent = b.id) AS _childCount,
              p.parent AS _grandParentId
       FROM moz_bookmarks b
       LEFT JOIN moz_bookmarks p ON p.id = b.parent
       LEFT JOIN moz_places h ON h.id = b.fk
       WHERE b.guid LIKE :guidPrefix
       ORDER BY b.lastModified DESC
      `,
      { guidPrefix: info.guidPrefix + "%" }
    );

    return rows.length ? rowsToItemsArray(rows) : null;
  };

  if (options.concurrent) {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    return query(db);
  }
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: fetchBookmarksByGUIDPrefix",
    query
  );
}

// eslint-disable-next-line no-unused-vars
async function fetchBookmarksByURLs(urls, options = {}) {
  if (!urls.length) {
    throw new Error("URLs array must not be empty");
  }
  let query = async function (db) {
    let params = {
      tagsFolderId: lazy.PlacesUtils.tagsFolderId,
      urls,
    };
    let rows = await db.executeCached(
      `/* do not warn (bug no): not worth to add an index */
      WITH urls(url, url_hash) AS (
        SELECT value, hash(value) FROM carray(:urls)
      )
      SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
              b.dateAdded, b.lastModified, b.type, IFNULL(b.title, '') AS title,
              h.url AS url, b.id AS _id, b.parent AS _parentId,
              NULL AS _childCount,
              p.parent AS _grandParentId,
              (SELECT group_concat(pp.title ORDER BY pp.title)
               FROM moz_bookmarks bb
               JOIN moz_bookmarks pp ON bb.parent = pp.id
               JOIN moz_bookmarks gg ON pp.parent = gg.id
               WHERE bb.fk = h.id
               AND gg.guid = '${Bookmarks.tagsGuid}'
              ) AS _tags
      FROM moz_bookmarks b
      JOIN moz_bookmarks p ON p.id = b.parent
      JOIN moz_places h ON h.id = b.fk
      JOIN urls ON urls.url = h.url AND urls.url_hash = h.url_hash
      WHERE _grandParentId <> :tagsFolderId
      ORDER BY h.url, b.lastModified DESC`,
      params
    );

    let map = new Map();
    for (let item of rowsToItemsArray(rows)) {
      let href = item.url.href;
      let arr = map.get(href);
      if (!arr) {
        map.set(href, (arr = []));
      }
      arr.push(item);
    }
    return map;
  };

  if (options.concurrent) {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    return query(db);
  }
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: fetchBookmarksByURLs",
    query
  );
}

async function fetchBookmarksByURL(info, options = {}) {
  let query = async function (db) {
    let rows = await db.executeCached(
      `/* do not warn (bug no): not worth to add an index */
      SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
              b.dateAdded, b.lastModified, b.type, IFNULL(b.title, '') AS title,
              h.url AS url, b.id AS _id, b.parent AS _parentId,
              NULL AS _childCount, /* Unused for now */
              p.parent AS _grandParentId,
              (SELECT group_concat(pp.title ORDER BY pp.title)
               FROM moz_bookmarks bb
               JOIN moz_bookmarks pp ON bb.parent = pp.id
               JOIN moz_bookmarks gg ON pp.parent = gg.id
               WHERE bb.fk = h.id
               AND gg.guid = '${Bookmarks.tagsGuid}'
              ) AS _tags
      FROM moz_bookmarks b
      JOIN moz_bookmarks p ON p.id = b.parent
      JOIN moz_places h ON h.id = b.fk
      WHERE h.url_hash = hash(:url) AND h.url = :url
      AND _grandParentId <> :tagsFolderId
      ORDER BY b.lastModified DESC
      `,
      {
        url: info.url.href,
        tagsFolderId: lazy.PlacesUtils.tagsFolderId,
      }
    );

    return rows.length ? rowsToItemsArray(rows) : null;
  };

  if (options.concurrent) {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    return query(db);
  }
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: fetchBookmarksByURL",
    query
  );
}

async function fetchBookmarksByParentGUID(info, options = {}) {
  let query = async function (db) {
    let rows = await db.executeCached(
      `SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
              b.dateAdded, b.lastModified, b.type, IFNULL(b.title, '') AS title,
              h.url AS url,
              b.id AS _id,
              b.parent AS _parentId,
              (SELECT count(*) FROM moz_bookmarks WHERE parent = b.id) AS _childCount,
              NULL AS _grandParentId
       FROM moz_bookmarks b
       LEFT JOIN moz_bookmarks p ON p.id = b.parent
       LEFT JOIN moz_places h ON h.id = b.fk
       WHERE p.guid = :parentGuid
       ORDER BY b.position ASC
      `,
      { parentGuid: info.parentGuid }
    );

    return rows.length ? rowsToItemsArray(rows) : null;
  };

  if (options.concurrent) {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    return query(db);
  }
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: fetchBookmarksByParentGUID",
    query
  );
}

function fetchRecentBookmarks(numberOfItems) {
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: fetchRecentBookmarks",
    async function (db) {
      let rows = await db.executeCached(
        `SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
                b.dateAdded, b.lastModified, b.type,
                IFNULL(b.title, '') AS title, h.url AS url, b.id AS _id,
                b.parent AS _parentId, NULL AS _childCount,
                NULL AS _grandParentId
        FROM moz_bookmarks b
        JOIN moz_bookmarks p ON p.id = b.parent
        JOIN moz_places h ON h.id = b.fk
        WHERE p.parent <> :tagsFolderId
        AND b.type = :type
        AND url_hash NOT BETWEEN hash("place", "prefix_lo")
                              AND hash("place", "prefix_hi")
        ORDER BY b.dateAdded DESC, b.ROWID DESC
        LIMIT :numberOfItems
        `,
        {
          tagsFolderId: lazy.PlacesUtils.tagsFolderId,
          type: Bookmarks.TYPE_BOOKMARK,
          numberOfItems,
        }
      );

      return rows.length ? rowsToItemsArray(rows) : [];
    }
  );
}

async function fetchBookmarksByParent(db, info) {
  let rows = await db.executeCached(
    `SELECT b.guid, IFNULL(p.guid, '') AS parentGuid, b.position AS 'index',
            b.dateAdded, b.lastModified, b.type, IFNULL(b.title, '') AS title,
            h.url AS url, b.id AS _id, b.parent AS _parentId,
            (SELECT count(*) FROM moz_bookmarks WHERE parent = b.id) AS _childCount,
            p.parent AS _grandParentId
     FROM moz_bookmarks b
     LEFT JOIN moz_bookmarks p ON p.id = b.parent
     LEFT JOIN moz_places h ON h.id = b.fk
     WHERE p.guid = :parentGuid
     ORDER BY b.position ASC
    `,
    { parentGuid: info.parentGuid }
  );

  return rowsToItemsArray(rows);
}


function removeBookmarks(items, options) {
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: removeBookmarks",
    async function (db) {
      let urls = [];

      await db.executeTransaction(async function transaction() {
        let parents = new Map();
        for (let item of items) {
          parents.set(item.parentGuid, item._parentId);

          if (item.type == Bookmarks.TYPE_FOLDER) {
            if (
              options.preventRemovalOfNonEmptyFolders &&
              item._childCount > 0
            ) {
              throw new Error("Cannot remove a non-empty folder.");
            }
            urls = urls.concat(
              await removeFoldersContents(db, [item.guid], options)
            );
          }
        }

        await db.executeCached(
          `DELETE FROM moz_bookmarks
           WHERE guid IN carray(:guids)`,
          { guids: items.map(item => item.guid) }
        );
        for (let [parentGuid, parentId] of parents.entries()) {
          await db.executeCached(
            `WITH positions(id, pos, seq) AS (
            SELECT id, position AS pos,
                   (row_number() OVER (ORDER BY position)) - 1 AS seq
            FROM moz_bookmarks
            WHERE parent = :parentId
          )
          UPDATE moz_bookmarks
            SET position = (SELECT seq FROM positions WHERE positions.id = moz_bookmarks.id)
            WHERE id IN (SELECT id FROM positions WHERE seq <> pos)
        `,
            { parentId }
          );

          await setAncestorsLastModified(
            db,
            parentGuid,
            new Date()
          );
        }

        for (let i = 0; i < items.length; i++) {
          const item = items[i];
          for (let j = i + 1; j < items.length; j++) {
            if (
              items[j]._parentId == item._parentId &&
              items[j].index > item.index
            ) {
              items[j].index--;
            }
          }
        }
      });

      urls = urls.concat(items.map(item => item.url).filter(item => item));
      if (urls.length) {
        await lazy.PlacesUtils.keywords.removeFromURLsIfNotBookmarked(urls);
      }
    }
  );
}


function reorderChildren(parent, orderedChildrenGuids, options) {
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: reorderChildren",
    db =>
      db.executeTransaction(async function () {
        const oldIndices = new Map();
        (
          await db.executeCached(
            `SELECT guid, position FROM moz_bookmarks WHERE parent = :parentId`,
            { parentId: parent._id }
          )
        ).forEach(r =>
          oldIndices.set(
            r.getResultByName("guid"),
            r.getResultByName("position")
          )
        );
        let lastIndex = 0,
          needReorder = false;
        for (let guid of orderedChildrenGuids) {
          let requestedIndex = oldIndices.get(guid);
          if (requestedIndex === undefined) {
            continue;
          }
          if (requestedIndex < lastIndex) {
            needReorder = true;
            break;
          }
          lastIndex = requestedIndex;
        }
        if (!needReorder) {
          return [];
        }

        const valuesFragment = orderedChildrenGuids
          .map((g, i) => `("${g}", ${i})`)
          .join();
        await db.execute(
          `UPDATE moz_bookmarks
           SET position = sorted.pos,
               lastModified = :lastModified
           FROM (
             WITH fixed(guid, pos) AS (
               VALUES ${valuesFragment}
             )
             SELECT b.id,
                    row_number() OVER (ORDER BY CASE WHEN fixed.pos IS NULL THEN 1 ELSE 0 END ASC, fixed.pos ASC, position ASC) - 1 AS pos
             FROM moz_bookmarks b
             LEFT JOIN fixed ON b.guid = fixed.guid
             WHERE parent = :parentId
           ) AS sorted
           WHERE sorted.id = moz_bookmarks.id`,
          {
            parentId: parent._id,
            lastModified: lazy.PlacesUtils.toPRTime(options.lastModified),
          }
        );

        await setAncestorsLastModified(
          db,
          parent.guid,
          options.lastModified
        );
        return (
          await fetchBookmarksByParent(db, {
            parentGuid: parent.guid,
          })
        ).map(c => {
          Object.defineProperty(c, "_oldIndex", {
            value: oldIndices.get(c.guid) || 0,
            enumerable: false,
            configurable: true,
          });
          return c;
        });
      })
  );
}


function mergeIntoNewObject(...sources) {
  let dest = {};
  for (let src of sources) {
    for (let prop of Object.getOwnPropertyNames(src)) {
      Object.defineProperty(
        dest,
        prop,
        Object.getOwnPropertyDescriptor(src, prop)
      );
    }
  }
  return dest;
}

function removeSameValueProperties(dest, src) {
  for (let prop in dest) {
    let remove = false;
    switch (prop) {
      case "lastModified":
      case "dateAdded":
        remove =
          src.hasOwnProperty(prop) &&
          dest[prop].getTime() == src[prop].getTime();
        break;
      case "url":
        remove = src.hasOwnProperty(prop) && dest[prop].href == src[prop].href;
        break;
      default:
        remove = dest[prop] == src[prop];
    }
    if (remove && prop != "guid") {
      delete dest[prop];
    }
  }
}

function rowsToItemsArray(rows, ignoreInvalidURLs = false) {
  return rows.map(row => {
    let item = {};
    for (let prop of ["guid", "index", "type", "title"]) {
      item[prop] = row.getResultByName(prop);
    }
    for (let prop of ["dateAdded", "lastModified"]) {
      let value = row.getResultByName(prop);
      if (value) {
        item[prop] = lazy.PlacesUtils.toDate(value);
      }
    }
    let parentGuid = row.getResultByName("parentGuid");
    if (parentGuid) {
      item.parentGuid = parentGuid;
    }
    let url = row.getResultByName("url");
    if (url) {
      try {
        item.url = new URL(url);
      } catch (ex) {
        if (!ignoreInvalidURLs) {
          throw ex;
        }
      }
    }


    for (let prop of [
      "_id",
      "_parentId",
      "_childCount",
      "_grandParentId",
    ]) {
      let val = row.getResultByName(prop);
      if (val !== null) {
        Object.defineProperty(item, prop, {
          value: val,
          enumerable: false,
          configurable: true,
        });
      }
    }

    try {
      let tags = row.getResultByName("_tags");
      Object.defineProperty(item, "_tags", {
        value: tags
          ? tags
              .split(",")
              .sort((a, b) => a.toLowerCase().localeCompare(b.toLowerCase()))
          : [],
        enumerable: false,
        configurable: true,
      });
    } catch (ex) {
    }

    return item;
  });
}

function validateBookmarkObject(name, input, behavior) {
  return lazy.PlacesUtils.validateItemProperties(
    name,
    lazy.PlacesUtils.BOOKMARK_VALIDATORS,
    input,
    behavior
  );
}

var setAncestorsLastModified = async function (db, folderGuid, time) {
  await db.executeCached(
    `WITH RECURSIVE
     ancestors(aid) AS (
       SELECT id FROM moz_bookmarks WHERE guid = :guid
       UNION ALL
       SELECT parent FROM moz_bookmarks
       JOIN ancestors ON id = aid
       WHERE type = :type
     )
     UPDATE moz_bookmarks SET lastModified = :time
     WHERE id IN ancestors
    `,
    {
      guid: folderGuid,
      type: Bookmarks.TYPE_FOLDER,
      time: lazy.PlacesUtils.toPRTime(time),
    }
  );

};

var removeFoldersContents = async function (db, folderGuids, options) {
  if (!folderGuids.length) {
    return [];
  }

  let itemsRemoved = [];
  for (let folderGuid of folderGuids) {
    let rows = await db.executeCached(
      `WITH RECURSIVE
       descendants(did) AS (
         SELECT b.id FROM moz_bookmarks b
         JOIN moz_bookmarks p ON b.parent = p.id
         WHERE p.guid = :folderGuid
         UNION ALL
         SELECT id FROM moz_bookmarks
         JOIN descendants ON parent = did
       )
       SELECT b.id AS _id, b.parent AS _parentId, b.position AS 'index',
              b.type, url, b.guid, p.guid AS parentGuid, b.dateAdded,
              b.lastModified, IFNULL(b.title, '') AS title,
              p.parent AS _grandParentId, NULL AS _childCount
       FROM descendants
       /* The usage of CROSS JOIN is not random, it tells the optimizer
          to retain the original rows order, so the hierarchy is respected */
       CROSS JOIN moz_bookmarks b ON did = b.id
       JOIN moz_bookmarks p ON p.id = b.parent
       LEFT JOIN moz_places h ON b.fk = h.id`,
      { folderGuid }
    );
    itemsRemoved = itemsRemoved.concat(rowsToItemsArray(rows, true));
  }

  if (itemsRemoved.length) {
    await db.executeCached(
      `DELETE FROM moz_bookmarks WHERE id IN carray(:removedIds)`,
      { removedIds: itemsRemoved.map(item => item._id) }
    );
  }



  let { source = Bookmarks.SOURCES.DEFAULT } = options;
  let notifications = [];
  for (let item of itemsRemoved.reverse()) {
    let isUntagging = item._grandParentId == lazy.PlacesUtils.tagsFolderId;
    let url = "";
    if (item.type == Bookmarks.TYPE_BOOKMARK) {
      url = item.hasOwnProperty("url") ? item.url.href : null;
    }
    notifications.push(
      new PlacesBookmarkRemoved({
        id: item._id,
        url,
        title: item.title,
        parentId: item._parentId,
        index: item.index,
        itemType: item.type,
        guid: item.guid,
        parentGuid: item.parentGuid,
        source,
        isTagging: isUntagging,
        isDescendantRemoval:
          !lazy.PlacesUtils.bookmarks.userContentRoots.includes(
            item.parentGuid
          ),
      })
    );

    if (isUntagging) {
      for (let entry of await fetchBookmarksByURL(item, true)) {
        notifications.push(
          new PlacesBookmarkTags({
            id: entry._id,
            itemType: entry.type,
            url,
            guid: entry.guid,
            parentGuid: entry.parentGuid,
            tags: entry._tags,
            lastModified: entry.lastModified,
            source,
            isTagging: false,
          })
        );
      }
    }
  }

  if (notifications.length) {
    PlacesObservers.notifyListeners(notifications);
  }

  return itemsRemoved.filter(item => "url" in item).map(item => item.url);
};

async function retrieveFullBookmarkPaths(guids, options = {}) {
  if (!guids || !guids.length) {
    throw new Error("No GUIDs provided to retrieveFullBookmarkPaths.");
  }

  let query = async function (db) {
    let rows = await db.executeCached(
      `WITH RECURSIVE parents(start_guid, guid, _id, _parent, title) AS
          (SELECT guid AS start_guid, guid, id AS _id, parent AS _parent,
                  IFNULL(title, '') AS title
           FROM moz_bookmarks
           WHERE guid IN carray(:guidArray)
           UNION ALL
           SELECT p.start_guid, b.guid, b.id AS _id, b.parent AS _parent,
                  IFNULL(b.title, '') AS title
           FROM moz_bookmarks b
           INNER JOIN parents p ON b.id = p._parent)
        SELECT * FROM parents WHERE guid != :rootGuid;
      `,
      { guidArray: guids, rootGuid: lazy.PlacesUtils.bookmarks.rootGuid }
    );

    let pathsByGuid = new Map();
    for (let r of rows) {
      let startGuid = r.getResultByName("start_guid");
      let node = {
        guid: r.getResultByName("guid"),
        title: r.getResultByName("title"),
      };

      if (!pathsByGuid.has(startGuid)) {
        pathsByGuid.set(startGuid, []);
      }
      pathsByGuid.get(startGuid).push(node);
    }

    for (let pathArray of pathsByGuid.values()) {
      pathArray.reverse();
    }

    return pathsByGuid;
  };

  if (options.concurrent) {
    let db = await lazy.PlacesUtils.promiseDBConnection();
    return query(db);
  }
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.sys.mjs: retrieveFullBookmarkPaths",
    query
  );
}

async function getBookmarkDetailMap(aGuids) {
  if (!aGuids.length) {
    return new Map();
  }
  return lazy.PlacesUtils.withConnectionWrapper(
    "Bookmarks.geBookmarkDetailMap",
    async db => {
      let entries = new Map();
      await db.executeCached(
        `
            SELECT
              b.guid,
              b.id,
              b.parent,
              IFNULL(h.frecency, 0),
              IFNULL(h.hidden, 0),
              IFNULL(h.visit_count, 0),
              h.last_visit_date,
              (
                SELECT group_concat(pp.title ORDER BY pp.title)
                FROM moz_bookmarks bb
                JOIN moz_bookmarks pp ON pp.id = bb.parent
                JOIN moz_bookmarks gg ON gg.id = pp.parent
                WHERE bb.fk = h.id
                AND gg.guid = '${Bookmarks.tagsGuid}'
              ),
              t.guid, t.id, t.title
            FROM moz_bookmarks b
            LEFT JOIN moz_places h ON h.id = b.fk
            LEFT JOIN moz_bookmarks t ON t.guid = target_folder_guid(h.url)
            WHERE b.guid IN carray(:guids)
            `,
        { guids: aGuids },
        row => {
          const lastVisitDate = row.getResultByIndex(6);
          entries.set(row.getResultByIndex(0), {
            id: row.getResultByIndex(1),
            parentId: row.getResultByIndex(2),
            frecency: row.getResultByIndex(3),
            hidden: row.getResultByIndex(4),
            visitCount: row.getResultByIndex(5),
            lastVisitDate: lastVisitDate
              ? lazy.PlacesUtils.toDate(lastVisitDate).getTime()
              : null,
            tags: row.getResultByIndex(7),
            targetFolderGuid: row.getResultByIndex(8),
            targetFolderItemId: row.getResultByIndex(9),
            targetFolderTitle: row.getResultByIndex(10),
          });
        }
      );
      return entries;
    }
  );
}

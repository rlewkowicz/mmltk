/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  AsyncShutdown: "resource://gre/modules/AsyncShutdown.sys.mjs",
});

ChromeUtils.defineLazyGetter(lazy, "console", () => {
  return console.createInstance({
    prefix: "NotificationDB",
    maxLogLevelPref: "dom.webnotifications.loglevel",
  });
});

export class NotificationDB {
  #shutdownInProgress = false;

  #queueDrainedPromise = null;
  #queueDrainedPromiseResolve = null;

  #byTag = Object.create(null);
  #notifications = Object.create(null);
  #loaded = false;
  #tasks = [];
  #runningTask = null;

  #storagePath = null;

  constructor() {
    if (this.#shutdownInProgress) {
      return;
    }

    this.#notifications = Object.create(null);
    this.#byTag = Object.create(null);
    this.#loaded = false;

    this.#tasks = []; 
    this.#runningTask = null;

    lazy.AsyncShutdown.profileChangeTeardown.addBlocker(
      "NotificationDB: Need to make sure that all notification messages are processed",
      () => this.#queueDrainedPromise
    );
  }

  filterNonAppNotifications(notifications) {
    let result = Object.create(null);
    for (let origin in notifications) {
      result[origin] = Object.create(null);
      let persistentNotificationCount = 0;
      for (let id in notifications[origin]) {
        if (notifications[origin][id].serviceWorkerRegistrationScope) {
          persistentNotificationCount++;
          result[origin][id] = notifications[origin][id];
        }
      }
      if (persistentNotificationCount == 0) {
        lazy.console.debug(
          `Origin ${origin} is not linked to an app manifest, deleting.`
        );
        delete result[origin];
      }
    }

    return result;
  }

  load() {
    const NOTIFICATION_STORE_DIR = PathUtils.profileDir;
    this.#storagePath = PathUtils.join(
      NOTIFICATION_STORE_DIR,
      "notificationstore.json"
    );
    var promise = IOUtils.readUTF8(this.#storagePath);
    return promise.then(
      data => {
        if (data.length) {
          this.#notifications = this.filterNonAppNotifications(
            JSON.parse(data)
          );
        }

        if (this.#notifications) {
          for (var origin in this.#notifications) {
            this.#byTag[origin] = Object.create(null);
            for (var id in this.#notifications[origin]) {
              var curNotification = this.#notifications[origin][id];
              if (curNotification.tag) {
                this.#byTag[origin][curNotification.tag] = curNotification;
              }
            }
          }
        }

        this.#loaded = true;
      },

      () => {
        this.#loaded = true;
        return this.#createStore(NOTIFICATION_STORE_DIR);
      }
    );
  }

  #createStore(directory) {
    var promise = IOUtils.makeDirectory(directory, {
      ignoreExisting: true,
    });
    return promise.then(this.createFile());
  }

  createFile() {
    return IOUtils.writeUTF8(this.#storagePath, "", {
      tmpPath: this.#storagePath + ".tmp",
    });
  }

  save() {
    var data = JSON.stringify(this.#notifications);
    return IOUtils.writeUTF8(this.#storagePath, data, {
      tmpPath: this.#storagePath + ".tmp",
    });
  }

  testGetRawMap() {
    return {
      notifications: this.#notifications,
      byTag: this.#byTag,
    };
  }

  #ensureLoaded() {
    if (!this.#loaded) {
      return this.load();
    }
    return Promise.resolve();
  }

  queueTask(operation, data) {
    lazy.console.debug(`Queueing task: ${operation}`);

    var defer = {};

    this.#tasks.push({
      operation,
      data,
      defer,
    });

    var promise = new Promise((resolve, reject) => {
      defer.resolve = resolve;
      defer.reject = reject;
    });

    if (!this.#runningTask) {
      lazy.console.debug("Task queue was not running, starting now...");
      this.runNextTask();
      this.#queueDrainedPromise = new Promise(resolve => {
        this.#queueDrainedPromiseResolve = resolve;
      });
    }

    return promise;
  }

  runNextTask() {
    if (this.#tasks.length === 0) {
      lazy.console.debug("No more tasks to run, queue depleted");
      this.#runningTask = null;
      if (this.#queueDrainedPromiseResolve) {
        this.#queueDrainedPromiseResolve();
      } else {
        lazy.console.debug(
          "#queueDrainedPromiseResolve was null somehow, no promise to resolve"
        );
      }
      return;
    }
    this.#runningTask = this.#tasks.shift();

    this.#ensureLoaded()
      .then(() => {
        var task = this.#runningTask;

        switch (task.operation) {
          case "getall":
            return this.taskGetAll(task.data);

          case "get":
            return this.taskGet(task.data);

          case "save":
            return this.taskSave(task.data);

          case "delete":
            return this.taskDelete(task.data);

          case "deleteAllExcept":
            return this.taskDeleteAllExcept(task.data);

          default:
            return Promise.reject(
              new Error(`Found a task with unknown operation ${task.operation}`)
            );
        }
      })
      .then(payload => {
        lazy.console.debug(`Finishing task: ${this.#runningTask.operation}`);
        this.#runningTask.defer.resolve(payload);
      })
      .catch(err => {
        lazy.console.debug(
          `Error while running ${this.#runningTask.operation}: ${err}`
        );
        this.#runningTask.defer.reject(err);
      })
      .then(() => {
        this.runNextTask();
      });
  }

  removeOriginIfEmpty(origin) {
    if (!Object.keys(this.#notifications[origin]).length) {
      delete this.#notifications[origin];
      delete this.#byTag[origin];
    }
  }

  taskGetAll(data) {
    let { origin, scope } = data;
    lazy.console.debug(
      `Task, getting all for the origin ${origin} and SWR scope ${scope}`
    );

    if (!this.#notifications[origin]) {
      return [];
    }

    if (data.tag) {
      let n = this.#byTag[origin][data.tag];
      if (n && n.serviceWorkerRegistrationScope === data.scope) {
        return [n];
      }
      return [];
    }

    let notifications = Object.values(this.#notifications[origin]).filter(
      n => n.serviceWorkerRegistrationScope === data.scope
    );
    return notifications;
  }

  taskGet(data) {
    let { origin, id } = data;
    lazy.console.debug(`Task, getting for the origin ${origin} and ID ${id}`);
    return this.#notifications[origin]?.[id];
  }

  taskSave(data) {
    lazy.console.debug("Task, saving");
    var origin = data.origin;
    var notification = data.notification;
    if (!this.#notifications[origin]) {
      this.#notifications[origin] = Object.create(null);
      this.#byTag[origin] = Object.create(null);
    }

    if (notification.tag) {
      var oldNotification = this.#byTag[origin][notification.tag];
      if (oldNotification) {
        delete this.#notifications[origin][oldNotification.id];
      }
      this.#byTag[origin][notification.tag] = notification;
    }

    this.#notifications[origin][notification.id] = notification;
    return this.save();
  }

  taskDelete(data) {
    lazy.console.debug("Task, deleting");
    var origin = data.origin;
    var id = data.id;
    if (!this.#notifications[origin]) {
      lazy.console.debug(`No notifications found for origin: ${origin}`);
      return Promise.resolve();
    }

    var oldNotification = this.#notifications[origin][id];
    if (!oldNotification) {
      lazy.console.debug(`No notification found with id: ${id}`);
      return Promise.resolve();
    }

    if (oldNotification.tag) {
      delete this.#byTag[origin][oldNotification.tag];
    }
    delete this.#notifications[origin][id];
    this.removeOriginIfEmpty(origin);
    return this.save();
  }

  taskDeleteAllExcept({ ids }) {
    lazy.console.debug("Task, deleting all");

    const entries = Object.entries(this.#notifications);
    for (const [origin, data] of entries) {
      const originEntries = Object.entries(data).filter(
        ([id]) => !ids.includes(id)
      );
      for (const [id, oldNotification] of originEntries) {
        delete data[id];
        if (oldNotification.tag) {
          delete this.#byTag[origin][oldNotification.tag];
        }
      }
      this.removeOriginIfEmpty(origin);
    }

    return this.save();
  }
}

export const db = new NotificationDB();

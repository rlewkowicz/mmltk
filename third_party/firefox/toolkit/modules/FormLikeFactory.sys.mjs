/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

export let FormLikeFactory = {
  _propsFromForm: ["action", "autocomplete", "ownerDocument"],

  createFromForm(aForm) {
    if (!HTMLFormElement.isInstance(aForm)) {
      throw new Error("createFromForm: aForm must be a HTMLFormElement");
    }

    let formLike = {
      elements: this.gatherFormElements(aForm),
      rootElement: aForm,
    };

    for (let prop of this._propsFromForm) {
      formLike[prop] = aForm[prop];
    }

    this._addToJSONProperty(formLike);

    return formLike;
  },

  gatherFormElements(aForm) {
    if (!aForm.querySelector("form")) {
      return [...aForm.elements];
    }

    let childElements = [...aForm.querySelectorAll("input, select")];
    childElements = childElements.filter(
      e => !e.getAttribute("form") || e.form == aForm
    );

    let index = 0;
    for (const formElement of aForm.elements) {
      if (!childElements.includes(formElement)) {
        let position = aForm.compareDocumentPosition(formElement);
        if (position & Node.DOCUMENT_POSITION_PRECEDING) {
          childElements.splice(index++, 0, formElement);
          continue;
        } else {
          childElements.push(formElement);
        }
      }
    }

    return childElements;
  },

  createFromDocumentRoot(aDocumentRoot, aOptions = {}) {
    if (!aDocumentRoot) {
      throw new Error("createFromDocumentRoot: aDocumentRoot is null");
    }

    let formLike = {
      action: aDocumentRoot.baseURI,
      autocomplete: "on",
      ownerDocument: aDocumentRoot.ownerDocument,
      rootElement: aDocumentRoot,
    };

    ChromeUtils.defineLazyGetter(formLike, "elements", function () {
      let elements = [];
      for (let el of aDocumentRoot.querySelectorAll(
        "input, select, textarea"
      )) {
        if (!el.form || aOptions.ignoreForm) {
          elements.push(el);
        }
      }

      return elements;
    });

    this._addToJSONProperty(formLike);
    return formLike;
  },

  createFromField(aField, aOptions = {}) {
    if (
      (!HTMLInputElement.isInstance(aField) &&
        !HTMLIFrameElement.isInstance(aField) &&
        !HTMLSelectElement.isInstance(aField) &&
        !HTMLTextAreaElement.isInstance(aField)) ||
      !aField.ownerDocument
    ) {
      throw new Error("createFromField requires a field in a document");
    }

    const rootElement = this.findRootForField(aField, aOptions);
    return HTMLFormElement.isInstance(rootElement)
      ? this.createFromForm(rootElement)
      : this.createFromDocumentRoot(rootElement, aOptions);
  },

  closestFormIgnoringShadowRoots(aField) {
    let form = aField.closest("form");
    let current = aField;
    while (!form) {
      let shadowRoot = current.getRootNode();
      if (!ShadowRoot.isInstance(shadowRoot)) {
        break;
      }
      let host = shadowRoot.host;
      form = host.closest("form");
      current = host;
    }
    return form;
  },

  findRootForField(aField, { ignoreForm = false } = {}) {
    if (!ignoreForm) {
      let form = aField.form || this.closestFormIgnoringShadowRoots(aField);
      if (form) {
        let parent = form;
        while ((parent = parent.parentNode)) {
          if (HTMLFormElement.isInstance(parent)) {
            form = parent;
          }
        }
        return form;
      }
    }

    return aField.ownerDocument.documentElement;
  },

  _addToJSONProperty(aFormLike) {
    function prettyElementOutput(aElement) {
      let idText = aElement.id ? "#" + aElement.id : "";
      let classText = "";
      for (let className of aElement.classList) {
        classText += "." + className;
      }
      return `<${aElement.nodeName + idText + classText}>`;
    }

    Object.defineProperty(aFormLike, "toJSON", {
      value: () => {
        let cleansed = {};
        for (let key of Object.keys(aFormLike)) {
          let value = aFormLike[key];
          let cleansedValue = value;

          switch (key) {
            case "elements": {
              cleansedValue = [];
              for (let element of value) {
                cleansedValue.push(prettyElementOutput(element));
              }
              break;
            }

            case "ownerDocument": {
              cleansedValue = {
                location: {
                  href: value.location.href,
                },
              };
              break;
            }

            case "rootElement": {
              cleansedValue = prettyElementOutput(value);
              break;
            }
          }

          cleansed[key] = cleansedValue;
        }
        return cleansed;
      },
    });
  },
};

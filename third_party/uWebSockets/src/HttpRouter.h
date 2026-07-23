
#ifndef UWS_HTTPROUTER_HPP
#define UWS_HTTPROUTER_HPP

#include <map>
#include <vector>
#include <cstring>
#include <string_view>
#include <string>
#include <algorithm>
#include <memory>
#include <utility>

#include <iostream>

#include "MoveOnlyFunction.h"

namespace uWS {

template <class USERDATA>
struct HttpRouter {
    static constexpr std::string_view ANY_METHOD_TOKEN = "*";
    static const uint32_t HIGH_PRIORITY = 0xd0000000, MEDIUM_PRIORITY = 0xe0000000, LOW_PRIORITY = 0xf0000000;

   private:
    USERDATA userData;
    static const unsigned int MAX_URL_SEGMENTS = 100;

    static const uint32_t HANDLER_MASK = 0x0fffffff;

    std::vector<MoveOnlyFunction<bool(HttpRouter*)>> handlers;

    std::string_view currentUrl;
    std::string_view urlSegmentVector[MAX_URL_SEGMENTS];
    int urlSegmentTop;

    struct Node {
        std::string name;
        std::vector<std::unique_ptr<Node>> children;
        std::vector<uint32_t> handlers;
        bool isHighPriority;

        Node(std::string name) : name(name) {}
    } root = {"rootNode"};

    int lexicalOrder(std::string& name) {
        if (!name.length()) {
            return 2;
        }
        if (name[0] == ':') {
            return 1;
        }
        if (name[0] == '*') {
            return 0;
        }
        return 2;
    }

    Node* getNode(Node* parent, std::string child, bool isHighPriority) {
        for (std::unique_ptr<Node>& node : parent->children) {
            if (node->name == child && node->isHighPriority == isHighPriority) {
                return node.get();
            }
        }

        std::unique_ptr<Node> newNode(new Node(child));
        newNode->isHighPriority = isHighPriority;
        return parent->children
            .emplace(std::upper_bound(parent->children.begin(), parent->children.end(), newNode,
                                      [parent, this](auto& a, auto& b) {
                                          if (a->isHighPriority != b->isHighPriority) {
                                              return a->isHighPriority;
                                          }

                                          return b->name.length() && (parent != &root) &&
                                                 (lexicalOrder(b->name) < lexicalOrder(a->name));
                                      }),
                     std::move(newNode))
            ->get();
    }

    struct RouteParameters {
        friend struct HttpRouter;

       private:
        std::string_view params[MAX_URL_SEGMENTS];
        int paramsTop;

        void reset() {
            paramsTop = -1;
        }

        void push(std::string_view param) {
            params[++paramsTop] = param;
        }

        void pop() {
            paramsTop--;
        }
    } routeParameters;

    inline void setUrl(std::string_view url) {
        currentUrl = url;
        urlSegmentTop = -1;
    }

    inline std::pair<std::string_view, bool> getUrlSegment(int urlSegment) {
        if (urlSegment > urlSegmentTop) {
            if (!currentUrl.length() || urlSegment > int(MAX_URL_SEGMENTS - 1)) {
                return {{}, true};
            }

            currentUrl.remove_prefix(1);

            auto segmentLength = currentUrl.find('/');
            if (segmentLength == std::string::npos) {
                segmentLength = currentUrl.length();

                urlSegmentVector[urlSegment] = currentUrl.substr(0, segmentLength);
                urlSegmentTop++;

                currentUrl = currentUrl.substr(segmentLength);
            } else {
                urlSegmentVector[urlSegment] = currentUrl.substr(0, segmentLength);
                urlSegmentTop++;

                currentUrl = currentUrl.substr(segmentLength);
            }
        }
        return {urlSegmentVector[urlSegment], false};
    }

    bool executeHandlers(Node* parent, int urlSegment, USERDATA& userData) {
        auto [segment, isStop] = getUrlSegment(urlSegment);

        if (isStop) {
            for (uint32_t handler : parent->handlers) {
                if (handlers[handler & HANDLER_MASK](this)) {
                    return true;
                }
            }
            return false;
        }

        for (auto& p : parent->children) {
            if (p->name.length() && p->name[0] == '*') {
                for (uint32_t handler : p->handlers) {
                    if (handlers[handler & HANDLER_MASK](this)) {
                        return true;
                    }
                }
            } else if (p->name.length() && p->name[0] == ':' && segment.length()) {
                routeParameters.push(segment);
                if (executeHandlers(p.get(), urlSegment + 1, userData)) {
                    return true;
                }
                routeParameters.pop();
            } else if (p->name == segment) {
                if (executeHandlers(p.get(), urlSegment + 1, userData)) {
                    return true;
                }
            }
        }
        return false;
    }

    uint32_t findHandler(std::string method, std::string pattern, uint32_t priority) {
        for (std::unique_ptr<Node>& node : root.children) {
            if (method == node->name) {
                setUrl(pattern);
                Node* n = node.get();
                for (int i = 0; !getUrlSegment(i).second; i++) {
                    std::string segment = std::string(getUrlSegment(i).first);
                    Node* next = nullptr;
                    for (std::unique_ptr<Node>& child : n->children) {
                        if (((segment.length() && child->name.length() && segment[0] == ':' && child->name[0] == ':') ||
                             child->name == segment) &&
                            child->isHighPriority == (priority == HIGH_PRIORITY)) {
                            next = child.get();
                            break;
                        }
                    }
                    if (!next) {
                        return UINT32_MAX;
                    }
                    n = next;
                }
                for (unsigned int i = 0; i < n->handlers.size(); i++) {
                    if ((n->handlers[i] & ~HANDLER_MASK) == priority) {
                        return n->handlers[i];
                    }
                }
                return UINT32_MAX;
            }
        }
        return UINT32_MAX;
    }

   public:
    HttpRouter() {
        getNode(&root, std::string(ANY_METHOD_TOKEN.data(), ANY_METHOD_TOKEN.length()), false);
    }

    std::pair<int, std::string_view*> getParameters() {
        return {routeParameters.paramsTop, routeParameters.params};
    }

    USERDATA& getUserData() {
        return userData;
    }

    bool route(std::string_view method, std::string_view url) {
        setUrl(url);
        routeParameters.reset();

        for (auto& p : root.children) {
            if (p->name == method) {
                if (executeHandlers(p.get(), 0, userData)) {
                    return true;
                } else {
                    break;
                }
            }
        }

        if (root.children.empty()) [[unlikely]] {
            return false;
        }
        return executeHandlers(root.children.back().get(), 0, userData);
    }

    void add(std::vector<std::string> methods, std::string pattern, MoveOnlyFunction<bool(HttpRouter*)>&& handler,
             uint32_t priority = MEDIUM_PRIORITY) {
        remove(methods[0], pattern, priority);

        for (std::string method : methods) {
            Node* node = getNode(&root, method, false);
            setUrl(pattern);
            for (int i = 0; !getUrlSegment(i).second; i++) {
                std::string strippedSegment(getUrlSegment(i).first);
                if (strippedSegment.length() && strippedSegment[0] == ':') {
                    strippedSegment = ":";
                }
                node = getNode(node, strippedSegment, priority == HIGH_PRIORITY);
            }
            node->handlers.insert(
                std::upper_bound(node->handlers.begin(), node->handlers.end(), (uint32_t)(priority | handlers.size())),
                (uint32_t)(priority | handlers.size()));
        }

        handlers.emplace_back(std::move(handler));

        std::sort(root.children.begin(), root.children.end(), [](const auto& a, const auto& b) {
            if (a->name == "GET" && b->name != "GET") {
                return true;
            } else if (b->name == "GET" && a->name != "GET") {
                return false;
            } else if (a->name == ANY_METHOD_TOKEN && b->name != ANY_METHOD_TOKEN) {
                return false;
            } else if (b->name == ANY_METHOD_TOKEN && a->name != ANY_METHOD_TOKEN) {
                return true;
            } else {
                return a->name < b->name;
            }
        });
    }

    bool cullNode(Node* parent, Node* node, uint32_t handler) {
        for (unsigned int i = 0; i < node->children.size();) {
            if (!cullNode(node, node->children[i].get(), handler)) {
                i++;
            }
        }

        if (parent) {
            for (auto it = node->handlers.begin(); it != node->handlers.end();) {
                if ((*it & HANDLER_MASK) > (handler & HANDLER_MASK)) {
                    *it = ((*it & HANDLER_MASK) - 1) | (*it & ~HANDLER_MASK);
                } else if (*it == handler) {
                    it = node->handlers.erase(it);
                    continue;
                }
                it++;
            }

            if (!node->handlers.size() && !node->children.size()) {
                parent->children.erase(
                    std::find_if(parent->children.begin(), parent->children.end(),
                                 [node](const std::unique_ptr<Node>& a) { return a.get() == node; }));
                return true;
            }
        }

        return false;
    }

    bool remove(std::string method, std::string pattern, uint32_t priority) {
        uint32_t handler = findHandler(method, pattern, priority);
        if (handler == UINT32_MAX) {
            return false;
        }

        cullNode(nullptr, &root, handler);

        handlers.erase(handlers.begin() + (handler & HANDLER_MASK));

        return true;
    }
};

}  

#endif  // UWS_HTTPROUTER_HPP

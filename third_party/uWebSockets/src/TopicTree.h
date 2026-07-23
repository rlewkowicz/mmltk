
#ifndef UWS_TOPICTREE_H
#define UWS_TOPICTREE_H

#include <map>
#include <list>
#include <iostream>
#include <unordered_set>
#include <utility>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string_view>
#include <functional>
#include <set>
#include <string>
#include <exception>

namespace uWS {

struct Subscriber;

struct Topic : std::unordered_set<Subscriber*> {
    Topic(std::string_view topic) : name(topic) {}

    std::string name;
};

struct Subscriber {
    template <typename, typename>
    friend struct TopicTree;

   private:
    Subscriber() = default;

    Subscriber *prev, *next;

    uint16_t messageIndices[32];

    unsigned char numMessageIndices = 0;

   public:
    std::set<Topic*> topics;

    void* user;

    bool needsDrainage() {
        return numMessageIndices;
    }
};

template <typename T, typename B>
struct TopicTree {
    enum IteratorFlags {
        NONE = 0,
        LAST = 1,
        FIRST = 2,
        FIRST_AND_LAST = FIRST | LAST
    };

    Subscriber* iteratingSubscriber = nullptr;

   private:
    std::function<bool(Subscriber*, T&, IteratorFlags)> cb;

    std::unordered_map<std::string_view, std::unique_ptr<Topic>> topics;

    Subscriber* drainableSubscribers = nullptr;

    std::vector<T> outgoingMessages;

    void checkIteratingSubscriber(Subscriber* s) {
        if (iteratingSubscriber == s) {
            std::cerr << "Error: WebSocket must not subscribe or unsubscribe to topics while iterating its topics!"
                      << std::endl;
            std::terminate();
        }
    }

    void drainImpl(Subscriber* s) {
        int numMessageIndices = s->numMessageIndices;
        s->numMessageIndices = 0;

        for (int i = 0; i < numMessageIndices; i++) {
            T& outgoingMessage = outgoingMessages[s->messageIndices[i]];

            IteratorFlags flags = NONE;
            if (i == 0 && i == numMessageIndices - 1) {
                flags = FIRST_AND_LAST;
            } else if (i == 0) {
                flags = FIRST;
            } else if (i == numMessageIndices - 1) {
                flags = LAST;
            }

            if (cb(s, outgoingMessage, flags)) {
                break;
            }
        }
    }

    void unlinkDrainableSubscriber(Subscriber* s) {
        if (s->prev) {
            s->prev->next = s->next;
        }
        if (s->next) {
            s->next->prev = s->prev;
        }
        if (drainableSubscribers == s) {
            drainableSubscribers = s->next;
        }
    }

   public:
    TopicTree(std::function<bool(Subscriber*, T&, IteratorFlags)> cb) : cb(cb) {}

    Topic* lookupTopic(std::string_view topic) {
        auto it = topics.find(topic);
        if (it == topics.end()) {
            return nullptr;
        }
        return it->second.get();
    }

    Topic* subscribe(Subscriber* s, std::string_view topic) {
        checkIteratingSubscriber(s);

        Topic* topicPtr = lookupTopic(topic);
        if (!topicPtr) {
            Topic* newTopic = new Topic(topic);
            topics.insert(
                {std::string_view(newTopic->name.data(), newTopic->name.length()), std::unique_ptr<Topic>(newTopic)});
            topicPtr = newTopic;
        }

        auto [it, inserted] = s->topics.insert(topicPtr);
        if (!inserted) {
            return nullptr;
        }
        topicPtr->insert(s);

        return topicPtr;
    }

    std::tuple<bool, bool, int> unsubscribe(Subscriber* s, std::string_view topic) {
        checkIteratingSubscriber(s);

        Topic* topicPtr = lookupTopic(topic);
        if (!topicPtr) {
            return {false, false, -1};
        }

        if (s->topics.erase(topicPtr) == 0) {
            return {false, false, -1};
        }

        topicPtr->erase(s);

        int newCount = (int)topicPtr->size();

        if (!topicPtr->size()) {
            topics.erase(topic);
        }

        return {true, s->topics.size() == 0, newCount};
    }

    Subscriber* createSubscriber() {
        return new Subscriber();
    }

    void freeSubscriber(Subscriber* s) {
        if (!s) {
            return;
        }

        for (Topic* topicPtr : s->topics) {
            if (topicPtr->size() == 1) {
                topics.erase(topicPtr->name);
            } else {
                topicPtr->erase(s);
            }
        }

        if (s->needsDrainage()) {
            unlinkDrainableSubscriber(s);
        }

        delete s;
    }

    void drain(Subscriber* s) {
        if (s->needsDrainage()) {
            unlinkDrainableSubscriber(s);

            drainImpl(s);

            if (!drainableSubscribers) {
                outgoingMessages.clear();
            }
        }
    }

    void drain() {
        if (drainableSubscribers) {
            for (Subscriber* s = drainableSubscribers; s; s = s->next) {
                drainImpl(s);
            }
            drainableSubscribers = nullptr;
            outgoingMessages.clear();
        }
    }

    template <typename F>
    bool publishBig(Subscriber* sender, std::string_view topic, B&& bigMessage, F cb) {
        auto it = topics.find(topic);
        if (it == topics.end()) {
            return false;
        }

        for (Subscriber* s : *it->second) {
            if (sender != s) {
                cb(s, bigMessage);
            }
        }

        return true;
    }

    bool publish(Subscriber* sender, std::string_view topic, T&& message) {
        auto it = topics.find(topic);
        if (it == topics.end()) {
            return false;
        }

        if (outgoingMessages.size() == UINT16_MAX) {
            drain();
        }

        bool referencedMessage = false;

        for (Subscriber* s : *it->second) {
            if (sender != s) {
                referencedMessage = true;

                if (s->numMessageIndices == 32) {
                    drain(s);
                }

                s->messageIndices[s->numMessageIndices++] = (uint16_t)outgoingMessages.size();
                if (s->numMessageIndices == 1) {
                    s->next = drainableSubscribers;
                    s->prev = nullptr;
                    if (s->next) {
                        s->next->prev = s;
                    }
                    drainableSubscribers = s;
                }
            }
        }

        if (referencedMessage) {
            outgoingMessages.emplace_back(message);
        }

        return referencedMessage;
    }
};

}  

#endif

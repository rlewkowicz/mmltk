
#ifndef SNI_TREE_H
#define SNI_TREE_H

#ifndef LIBUS_NO_SSL

#include <map>
#include <memory>
#include <string_view>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#define MAX_LABELS 10

thread_local void (*sni_free_cb)(void*);

struct sni_node {
    void* user = nullptr;
    std::map<std::string_view, std::unique_ptr<sni_node>> children;

    ~sni_node() {
        for (auto& p : children) {
            free((void*)p.first.data());

            if (p.second.get()->user) {
                sni_free_cb(p.second.get()->user);
            }
        }
    }
};

void* removeUser(struct sni_node* root, unsigned int label, std::string_view* labels, unsigned int numLabels) {
    if (label == numLabels) {
        void* user = root->user;
        root->user = nullptr;
        return user;
    }

    auto it = root->children.find(labels[label]);
    if (it == root->children.end()) {
        return nullptr;
    }

    void* removedUser = removeUser(it->second.get(), label + 1, labels, numLabels);

    if (it->second.get()->children.empty() && it->second.get()->user == nullptr) {
        free((void*)it->first.data());

        root->children.erase(it);
    }

    return removedUser;
}

void* getUser(struct sni_node* root, unsigned int label, std::string_view* labels, unsigned int numLabels) {
    if (label == numLabels) {
        return root->user;
    }

    auto it = root->children.find(labels[label]);
    if (it != root->children.end()) {
        void* user = getUser(it->second.get(), label + 1, labels, numLabels);
        if (user) {
            return user;
        }
    }

    it = root->children.find("*");
    if (it == root->children.end()) {
        return nullptr;
    }

    return getUser(it->second.get(), label + 1, labels, numLabels);
}

bool sni_split_labels(const char* hostname, std::string_view (&labels)[MAX_LABELS], unsigned int& numLabels) {
    numLabels = 0;
    for (std::string_view view(hostname, strlen(hostname)), label; view.length();
         view.remove_prefix(std::min(view.length(), label.length() + 1))) {
        label = view.substr(0, view.find('.', 0));
        if (numLabels == MAX_LABELS) {
            return false;
        }
        labels[numLabels++] = label;
    }
    return true;
}

extern "C" {

void* sni_new() {
    return new sni_node;
}

void sni_free(void* sni, void (*cb)(void*)) {
    sni_free_cb = cb;

    delete (sni_node*)sni;
}

int sni_add(void* sni, const char* hostname, void* user) {
    struct sni_node* root = (struct sni_node*)sni;

    for (std::string_view view(hostname, strlen(hostname)), label; view.length();
         view.remove_prefix(std::min(view.length(), label.length() + 1))) {
        label = view.substr(0, view.find('.', 0));

        auto it = root->children.find(label);
        if (it == root->children.end()) {
            void* labelString = malloc(label.length());
            memcpy(labelString, label.data(), label.length());

            it = root->children
                     .emplace(std::string_view((char*)labelString, label.length()), std::make_unique<sni_node>())
                     .first;
        }

        root = it->second.get();
    }

    if (root->user) {
        return 1;
    }

    root->user = user;

    return 0;
}

void* sni_remove(void* sni, const char* hostname) {
    struct sni_node* root = (struct sni_node*)sni;

    std::string_view labels[10];
    unsigned int numLabels = 0;

    if (!sni_split_labels(hostname, labels, numLabels)) {
        return nullptr;
    }

    return removeUser(root, 0, labels, numLabels);
}

void* sni_find(void* sni, const char* hostname) {
    struct sni_node* root = (struct sni_node*)sni;

    std::string_view labels[10];
    unsigned int numLabels = 0;

    if (!sni_split_labels(hostname, labels, numLabels)) {
        return nullptr;
    }

    return getUser(root, 0, labels, numLabels);
}
}

#endif

#endif

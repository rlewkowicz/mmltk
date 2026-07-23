
#ifndef UWS_LOOP_H
#define UWS_LOOP_H

#include "LoopData.h"
#include <libusockets.h>
#include <iostream>

namespace uWS {

struct PreparedMessage {
    std::string originalMessage, compressedMessage;
    bool compressed;
    int opCode;
};

struct Loop {
   private:
    static void wakeupCb(us_loop_t* loop) {
        LoopData* loopData = (LoopData*)us_loop_ext(loop);

        loopData->deferMutex.lock();
        int oldDeferQueue = loopData->currentDeferQueue;
        loopData->currentDeferQueue = (loopData->currentDeferQueue + 1) % 2;
        loopData->deferMutex.unlock();

        for (auto& x : loopData->deferQueues[oldDeferQueue]) {
            x();
        }
        loopData->deferQueues[oldDeferQueue].clear();
    }

    static void preCb(us_loop_t* loop) {
        LoopData* loopData = (LoopData*)us_loop_ext(loop);

        for (auto& p : loopData->preHandlers) {
            p.second((Loop*)loop);
        }
    }

    static void postCb(us_loop_t* loop) {
        LoopData* loopData = (LoopData*)us_loop_ext(loop);

        for (auto& p : loopData->postHandlers) {
            p.second((Loop*)loop);
        }

        if (loopData->corkedSocket) {
            std::cerr << "Error: Cork buffer must not be held across event loop iterations!" << std::endl;
            std::terminate();
        }
    }

    Loop() = delete;
    ~Loop() = default;

    Loop* init() {
        new (us_loop_ext((us_loop_t*)this)) LoopData;
        return this;
    }

    static Loop* create(void* hint) {
        Loop* loop = ((Loop*)us_create_loop(hint, wakeupCb, preCb, postCb, sizeof(LoopData)))->init();

        LoopData* loopData = (LoopData*)us_loop_ext((struct us_loop_t*)loop);
        loopData->dateTimer = us_create_timer((struct us_loop_t*)loop, 1, sizeof(LoopData*));
        memcpy(us_timer_ext(loopData->dateTimer), &loopData, sizeof(LoopData*));
        us_timer_set(
            loopData->dateTimer,
            [](struct us_timer_t* t) {
                LoopData* loopData;
                memcpy(&loopData, us_timer_ext(t), sizeof(LoopData*));
                loopData->updateDate();
            },
            1000, 1000);

        return loop;
    }

    struct LoopCleaner {
        ~LoopCleaner() {
            if (loop && cleanMe) {
                loop->free();
            }
        }
        Loop* loop = nullptr;
        bool cleanMe = false;
    };

    static LoopCleaner& getLazyLoop() {
        static thread_local LoopCleaner lazyLoop;
        return lazyLoop;
    }

   public:
    PreparedMessage prepareMessage(std::string_view message, int opCode, bool compress = true) {
        PreparedMessage preparedMessage;
        preparedMessage.compressed = compress;
        preparedMessage.opCode = opCode;
        preparedMessage.originalMessage = message;

        LoopData* loopData = (LoopData*)us_loop_ext((us_loop_t*)this);

        if (compress) {
            if (!loopData->zlibContext) {
                loopData->zlibContext = new ZlibContext;
                loopData->inflationStream = new InflationStream(CompressOptions::DEDICATED_DECOMPRESSOR);
                loopData->deflationStream = new DeflationStream(CompressOptions::DEDICATED_COMPRESSOR);
            }

            preparedMessage.compressedMessage = loopData->deflationStream->deflate(
                loopData->zlibContext,
                {preparedMessage.originalMessage.data(), preparedMessage.originalMessage.length()}, true);
        }

        return preparedMessage;
    }

    static Loop* get(void* existingNativeLoop = nullptr) {
        if (!getLazyLoop().loop) {
            if (existingNativeLoop) {
                getLazyLoop().loop = create(existingNativeLoop);
            } else {
                getLazyLoop().loop = create(nullptr);
                getLazyLoop().cleanMe = true;
            }
        }

        return getLazyLoop().loop;
    }

    void free() {
        LoopData* loopData = (LoopData*)us_loop_ext((us_loop_t*)this);

        us_timer_close(loopData->dateTimer);

        loopData->~LoopData();
        us_loop_free((us_loop_t*)this);

        getLazyLoop().loop = nullptr;
    }

    void addPostHandler(void* key, MoveOnlyFunction<void(Loop*)>&& handler) {
        LoopData* loopData = (LoopData*)us_loop_ext((us_loop_t*)this);

        loopData->postHandlers.emplace(key, std::move(handler));
    }

    void removePostHandler(void* key) {
        LoopData* loopData = (LoopData*)us_loop_ext((us_loop_t*)this);

        loopData->postHandlers.erase(key);
    }

    void addPreHandler(void* key, MoveOnlyFunction<void(Loop*)>&& handler) {
        LoopData* loopData = (LoopData*)us_loop_ext((us_loop_t*)this);

        loopData->preHandlers.emplace(key, std::move(handler));
    }

    void removePreHandler(void* key) {
        LoopData* loopData = (LoopData*)us_loop_ext((us_loop_t*)this);

        loopData->preHandlers.erase(key);
    }

    void defer(MoveOnlyFunction<void()>&& cb) {
        LoopData* loopData = (LoopData*)us_loop_ext((us_loop_t*)this);

        loopData->deferMutex.lock();
        loopData->deferQueues[loopData->currentDeferQueue].emplace_back(std::move(cb));
        loopData->deferMutex.unlock();

        us_wakeup_loop((us_loop_t*)this);
    }

    void run() {
        us_loop_run((us_loop_t*)this);
    }

    void integrate() {
        us_loop_integrate((us_loop_t*)this);
    }

    void setSilent(bool silent) {
        ((LoopData*)us_loop_ext((us_loop_t*)this))->noMark = silent;
    }
};

inline void run() {
    Loop::get()->run();
}

}  

#endif  // UWS_LOOP_H

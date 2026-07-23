
#ifndef UWS_LOOPDATA_H
#define UWS_LOOPDATA_H

#include <thread>
#include <functional>
#include <vector>
#include <mutex>
#include <map>
#include <ctime>
#include <cstdint>

#include "PerMessageDeflate.h"
#include "MoveOnlyFunction.h"

struct us_timer_t;

namespace uWS {

struct Loop;

struct alignas(16) LoopData {
    friend struct Loop;

   private:
    std::mutex deferMutex;
    int currentDeferQueue = 0;
    std::vector<MoveOnlyFunction<void()>> deferQueues[2];

    std::map<void*, MoveOnlyFunction<void(Loop*)>> postHandlers, preHandlers;

   public:
    LoopData() {
        updateDate();
    }

    ~LoopData() {
        if (zlibContext) {
            delete zlibContext;
            delete inflationStream;
            delete deflationStream;
        }
        delete[] corkBuffer;
    }

    void updateDate() {
        cacheTimepoint = time(0);
        struct tm tstruct = {};
#ifdef _WIN32
        gmtime_s(&tstruct, &cacheTimepoint);
#else
        gmtime_r(&cacheTimepoint, &tstruct);
#endif
        static const char wday_name[][4] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char mon_name[][4] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        snprintf(date, 32, "%.3s, %.2u %.3s %.4u %.2u:%.2u:%.2u GMT", wday_name[tstruct.tm_wday], tstruct.tm_mday % 99,
                 mon_name[tstruct.tm_mon], (1900 + tstruct.tm_year) % 9999, tstruct.tm_hour % 99, tstruct.tm_min % 99,
                 tstruct.tm_sec % 99);
    }

    char date[32];
    time_t cacheTimepoint = 0;

    bool noMark = false;

    static const unsigned int CORK_BUFFER_SIZE = 16 * 1024;

    char* corkBuffer = new char[CORK_BUFFER_SIZE];
    unsigned int corkOffset = 0;
    void* corkedSocket = nullptr;

    ZlibContext* zlibContext = nullptr;
    InflationStream* inflationStream = nullptr;
    DeflationStream* deflationStream = nullptr;

    us_timer_t* dateTimer;
};

}  

#endif  // UWS_LOOPDATA_H

/*
   Copyright 2021 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef AX25MS_INCLUDE_SCHEDULER_H__
#define AX25MS_INCLUDE_SCHEDULER_H__

#include "proto/gen/ax25.pb.h"
#include <condition_variable>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace ax25ms {

class Retransmitter
{
};


class Timer
{
public:
    using duration_t = std::chrono::duration<int, std::milli>;
    using time_point_t = std::chrono::time_point<std::chrono::steady_clock>;
    using cb_t = std::function<void()>;

    Timer();
    ~Timer();

    void add(duration_t, cb_t cb);
    void add(time_point_t, cb_t cb);
    void run();

    time_point_t now();

    void stop_for_test() noexcept;
    void tick_for_test(duration_t d);

    void drain();

private:
    struct timer_t {
        time_point_t timepoint; // Time it should trigger.
        cb_t cb;                // Callback on trigger.
    };
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<time_point_t, std::unique_ptr<timer_t>> timers_;
    std::atomic<bool> shutdown_ = false;

    bool stop_for_test_ = false;
    time_point_t time_for_test_{};
    std::atomic<bool> running_ = false;

    // thread_ needs to be *last* so that it waits for everything else
    // to be constructed.
    std::jthread thread_;
};
} // namespace ax25ms
#endif

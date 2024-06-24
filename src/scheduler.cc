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
/*
 * Generic timer scheduler used by packet scheduler to trigger resend, etc.
 */
#include "scheduler.h"
namespace ax25ms {

Timer::Timer() : thread_([this] { run(); }) {}

Timer::~Timer()
{
    shutdown_ = true;
    cv_.notify_all();
}

void Timer::run()
{
    pthread_setname_np(thread_.native_handle(), "timer");
    for (;;) {
        auto getcb = [this] {
            std::unique_lock<std::mutex> lk(mu_);
            std::unique_ptr<timer_t> ret;
            for (;;) {
                if (timers_.empty()) {
                    cv_.wait(lk, [this] { return !timers_.empty() || shutdown_; });
                }
                if (shutdown_) {
                    return ret;
                }
                auto cur = timers_.begin()->first;

                if (stop_for_test_) {
                    auto noww = now();
                    if (noww >= cur) {
                        break;
                    }
                    mu_.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
                    mu_.lock();
                    continue;
                }

                const bool notimeout = cv_.wait_until(lk, cur, [this, &cur] {
                    auto maybe_new = timers_.begin()->first;
                    return maybe_new != cur;
                });
                if (notimeout) {
                    // Most recent timeout changed. Reevaluate how long to sleep.
                    continue;
                }
                break;
            }
            // Timeout has happened for the first item.
            auto first = timers_.begin();
            ret = std::move(first->second);

            timers_.erase(first);
            return ret;
        }();
        if (shutdown_) {
            break;
        }
        running_ = true;
        getcb->cb();
        running_ = false;
    }
}

void Timer::add(duration_t dur, cb_t cb)
{
    const auto t = [this, dur] {
        std::unique_lock<std::mutex> lk(mu_);
        return now() + dur;
    }();
    add(t, cb);
}

void Timer::stop_for_test() noexcept
{
    time_for_test_ = std::chrono::steady_clock::now();
    stop_for_test_ = true;
}

Timer::time_point_t Timer::now()
{
    if (stop_for_test_) {
        return time_for_test_;
    }
    return std::chrono::steady_clock::now();
}

void Timer::drain()
{
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
        std::unique_lock<std::mutex> lk(mu_);
        if (timers_.empty() && !running_) {
            return;
        }
    }
}

void Timer::tick_for_test(duration_t t)
{
    {
        std::unique_lock<std::mutex> lk(mu_);
        time_for_test_ = now() + t;
        cv_.notify_all();
    }

    // Wait for all scheduled handlers to run.
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            // Timer is running. Don't know when it was supposed to.
            if (running_) {
                continue;
            }
            // No timer left.
            if (timers_.empty()) {
                return;
            }
            // No timer left that's left to trigger.
            if (timers_.begin()->first > now()) {
                return;
            }
        }
    }
}

void Timer::add(time_point_t tp, cb_t cb)
{
    std::unique_lock<std::mutex> lk(mu_);
    auto e = std::make_unique<timer_t>();
    e->timepoint = tp;
    e->cb = cb;
    timers_[tp] = std::move(e);
    cv_.notify_one();
}

} // namespace ax25ms

#if 0
#include <unistd.h>
#include <iostream>
#include <thread>
int main()
{
  ax25ms::Timer timer;
  std::jthread th([&timer]{timer.run();});

  std::cout << "Starting\n";
  using namespace std::chrono_literals;
  timer.add(1000ms, []{
    std::cout << "one second\n";
  });
  timer.add(2000ms, []{
    std::cout << "2 second\n";
  });
  timer.add(500ms, []{
    std::cout << "0.5 second\n";
  });
  sleep(10);
  return 0;
}
#endif

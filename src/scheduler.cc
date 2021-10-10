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
#include "scheduler.h"
namespace ax25ms {

Timer::Timer() : thread_([this] { run(); }) {}

void Timer::run()
{
    for (;;) {
        auto getcb = [this] {
            std::unique_lock<std::mutex> lk(mu_);
            for (;;) {
                if (timers_.empty()) {
                    cv_.wait(lk, [this] { return !timers_.empty(); });
                }
                auto cur = timers_.begin()->first;
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
            auto entry = std::move(first->second);
            timers_.erase(first);
            return entry;
        };
        getcb()->cb();
    }
}

void Timer::add(duration_t dur, cb_t cb)
{
    add(std::chrono::steady_clock::now() + dur, cb);
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

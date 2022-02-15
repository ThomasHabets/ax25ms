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
#ifndef __AX25MS_H_INCLUDE__
#define __AX25MS_H_INCLUDE__

#include <google/protobuf/message.h>

#include <string_view>
#include <sstream>
#include <string>

namespace ax25ms {

class LogLine
{
public:
    LogLine();

    // No copy.
    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

    // Move ok.
    LogLine(LogLine&&);
    LogLine& operator=(LogLine&&);

    template <typename T>
    LogLine& operator<<(const T& v)
    {
        s_ << v;
        return *this;
    }
    ~LogLine();
    void set_output(std::ostream* o) noexcept { out_ = o; };

private:
    time_t system_now_;
    std::ostream* out_;
    std::stringstream s_;
};

LogLine log();
std::string proto2string(const google::protobuf::Message& proto);

std::string str2hex(std::string_view data);

} // namespace ax25ms
#endif

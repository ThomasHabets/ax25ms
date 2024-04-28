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
#include "util.h"

#include <google/protobuf/text_format.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
namespace ax25ms {

std::string proto2string(const google::protobuf::Message& proto)
{
    std::string str;
    google::protobuf::TextFormat::PrintToString(proto, &str);
    return str;
}

std::string str2hex(std::string_view data)
{
    std::stringstream ss;
    for (auto ch : data) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << (static_cast<unsigned int>(ch) & 0xff) << " ";
    }
    return ss.str();
}

LogLine::LogLine() : system_now_(std::time(nullptr)), out_(&std::cerr) {}

LogLine::~LogLine()
{
    std::stringstream ss;
    ss << "ax25ms " << std::put_time(std::localtime(&system_now_), "%F %T") << " "
       << s_.str() << "\n";

    if (!out_) {
        // A benefit of writing to /dev/null is that it'll still be
        // visible in strace.
        static std::mutex mu;
        static std::ofstream null("/dev/null");
        std::unique_lock<std::mutex> lk(mu);
        null << ss.str();
    } else {
        static std::mutex mu;
        std::unique_lock<std::mutex> lk(mu);
        *out_ << ss.str();
    }
}

LogLine::LogLine(LogLine&& rhs)
    : system_now_(rhs.system_now_), out_(std::exchange(rhs.out_, nullptr))
{
}

LogLine& LogLine::operator=(LogLine&& rhs)
{
    system_now_ = rhs.system_now_;
    out_ = std::exchange(rhs.out_, nullptr);
    return *this;
}

LogLine log() { return LogLine(); }

} // namespace ax25ms

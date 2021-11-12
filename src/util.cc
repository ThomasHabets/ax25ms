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

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
namespace ax25ms {

std::string str2hex(std::string_view data)
{
    std::stringstream ss;
    for (auto ch : data) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << (static_cast<unsigned int>(ch) & 0xff) << " ";
    }
    return ss.str();
}

} // namespace ax25ms

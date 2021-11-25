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

#include <iostream>
#include <map>
#include <string>

namespace ax25ms::serial {
std::string kiss_escape(std::string_view sv);
std::string kiss_unescape(std::string_view sv);
} // namespace ax25ms::serial

using namespace ax25ms::serial;
using ax25ms::str2hex;

int main()
{
    std::map<std::string, std::string> escape_test{
        { "", "" },
        { "hello", "hello" },
        { "\xC0", "\xDB\xDC" },
        { "\xC0\xC0", "\xDB\xDC\xDB\xDC" },
        { "\xDB\xC0", "\xDB\xDD\xDB\xDC" },
        { "Abc\xC0", "Abc\xDB\xDC" },
        { "Abc\xC0"
          "12",
          "Abc\xDB\xDC"
          "12" },
        { "\xC0"
          "12",
          "\xDB\xDC"
          "12" },
    };
    int ret = 0;
    for (const auto [s, want] : escape_test) {
        if (auto got = kiss_escape(s); got != want) {
            std::cerr << "For " << str2hex(s) << ": got " << str2hex(got) << " want "
                      << str2hex(want) << "\n";
            ret = 1;
        } else {
            if (auto back = kiss_unescape(got); back != s) {
                std::cerr << "For " << str2hex(s) << " via " << str2hex(got)
                          << ": got back " << str2hex(back) << "\n";
                ret = 1;
            }
        }
    }
    std::map<std::string, std::string> unescape_test{
        { "", "" },     { "hello", "hello" }, { "\xDB", "" },
        { "\xC0", "" }, { "hello\xDB", "" },  { "hello\xC0", "" },
    };
    for (const auto [s, want] : unescape_test) {
        if (auto got = kiss_unescape(s); got != want) {
            std::cerr << "Unesc: for " << str2hex(s) << ": got " << str2hex(got)
                      << " want " << str2hex(want) << "\n";
            ret = 1;
        }
    }
    return ret;
}

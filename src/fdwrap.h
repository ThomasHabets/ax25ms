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
#include <unistd.h>

class FDWrap
{
public:
    explicit FDWrap(int fd) : fd_(fd) {}
    FDWrap(const FDWrap&) = delete;
    FDWrap(FDWrap&& rhs)
    {
        fd_ = rhs.fd_;
        rhs.fd_ = -1;
    }

    int get() const noexcept { return fd_; }
    void close()
    {
        if (fd_ == -1) {
            return;
        }
        ::close(fd_);
        fd_ = -1;
    }
    ~FDWrap() { close(); }

private:
    int fd_ = -1;
};

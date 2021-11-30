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
 * Replacement state machine for seqpacket.
 */
#include "proto/gen/ax25.pb.h"

#include <string_view>
#include <deque>
#include <iostream>
#include <memory>
// TODO: implement timer.

#include <grpcpp/grpcpp.h>

namespace seqpacket::con {
class Connection;
class Timer
{
public:
    Timer(std::string_view sv) : name_(sv) {}
    void set(int ms) { ms_ = ms; }
    int get() { return ms_; }
    void start()
    {
        std::cerr << "Starting timer " << name_ << "\n";
        running_ = true;
    }
    void stop()
    {
        std::cerr << "Stopping timer " << name_ << "\n";
        running_ = false;
    }
    void restart()
    {
        stop();
        start();
    }
    bool running() { return running_; }

private:
    int ms_ = -1;
    bool running_ = false;
    const std::string name_;
};

struct ConnectionData {
    int modulus = 0;

    // Flags (Page 82)
    bool layer3_initiated = false;
    bool peer_receiver_busy = false;
    bool own_receiver_busy = false;
    bool reject_exception = false;
    bool selective_reject_exception = false;
    bool acknowledge_pending = false;
    double srt = 0;
    int t1v = 0;  // Next value for T1; default init value is initial value of SRT.
    int n1 = 200; // Max number of octets in the information field of a frame.
    int n2 = 3;   // Max number of retries permitted.

    Timer t1{ "t1" }; // Outstanding I frame or P-bit
    Timer t2{ "t2" }; // Response delay timer. 6.7.1.2
    Timer t3{ "t3" }; // Idle supervision (keep alive).

    int rc{};
    // int nr{};

    // 6.7.2.3. Maximum Number of I Frames Outstanding (k)
    //
    // The maximum number of I frames outstanding at a time is seven (modulo 8) or
    // 127 (modulo 128).
    int k = 7;

    // 4.2.4.1: Send State Variable.
    //
    // The send state variable exists within the TNC and is never sent. It
    // contains the next sequential number to be assigned to the next transmitted
    // I frame. This variable is updated with the transmission of each I frame.
    int vs{};

    // 4.2.4.5. Acknowledge State Variable V(A)
    //
    // The acknowledge state variable exists within the TNC and is never sent. It
    // contains the sequence number of the last frame acknowledged by its peer
    // [V(A)-1 equals the N(S) of the last acknowledged I frame].
    int va{};

    // RC = retry count?

    // 4.2.4.3. Receive State Variable V(R)
    //
    // The receive state variable exists within the TNC. It contains the sequence number
    // of the next expected received I frame. This variable is updated upon the reception
    // of an error-free I frame whose send sequence number equals the present received
    // state variable value
    int vr{};

    bool srej_enabled = false;
    int sreject_exception{};

    // Queues (page 81).
    std::deque<ax25::Packet> iframe_queue_;
};

class ConnectionState
{
public:
    using stateptr_t = std::unique_ptr<ConnectionState>;

    // Page 81.
    enum class DLError {
        A,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K,
        L,
        M,
        N,
        O,
        P,
        Q,
        R,
        S,
        T,
        U,
        V,
    };

    ConnectionState(Connection* connection);
    static constexpr int default_srt = 3000;

    virtual std::string name() { return "base"; }

    // Incoming packets.
    virtual stateptr_t ua(const ax25::Packet& p);
    virtual stateptr_t dm(const ax25::Packet& p);
    virtual stateptr_t disc(const ax25::Packet& p);
    virtual stateptr_t sabm(const ax25::Packet& p);
    virtual stateptr_t sabme(const ax25::Packet& p);
    virtual stateptr_t frmr(const ax25::Packet& p);
    virtual stateptr_t rr(const ax25::Packet& p);
    virtual stateptr_t ui(const ax25::Packet& p);
    virtual stateptr_t iframe(const ax25::Packet& p);
    virtual stateptr_t timer1_tick() { return nullptr; }

    // Request establishment of AX.25.
    virtual stateptr_t dl_connect() { return nullptr; }

    // Transmit data using connection oriented protocol.
    virtual stateptr_t dl_data(std::string_view sv) { return nullptr; }

    // Transmit data using connectionless protocol.
    // virtual stateptr_t dl_unit_data() { return nullptr; };

    // Release AX.25 connection.
    virtual stateptr_t dl_disconnect() { return nullptr; }

    stateptr_t connected_timer_recovery_disc(const ax25::Packet& p);
    void iframe_pop();

protected:
    // senders.
    void send_sabm(bool poll);
    void send_sabme(bool poll);
    void send_ua(bool poll);
    void send_dm(bool poll);
    void send_rr(bool poll, int nr);
    void send_rnr(bool poll, int nr);
    void send_rej(bool poll, int nr);
    void send_srej(bool poll, int nr);


    // subroutines.
    void nr_error_recovery();
    void establish_data_link();
    void clear_exception_conditions();
    void transmit_enquiry(int rc);
    void enquiry_response(bool f);
    void invoke_retransmission(int nr);
    void check_iframe_acked(int nr);
    void check_need_for_response(bool command, bool pf);
    void ui_check(bool command);
    void select_t1_value();
    void establish_extended_data_link();
    void set_version_2();
    void set_version_2_2();

    ConnectionData& d;
    Connection* connection_;

    // error reporting
    void dl_error(const DLError& e);
};

class Connection
{
public:
    using send_func_t = std::function<grpc::Status(const ax25::Packet&)>;

    Connection(send_func_t send);
#define P(xx)                            \
    template <typename T>                \
    void xx(const T& p)                  \
    {                                    \
        if (auto n = state_->xx(p); n) { \
            state_ = std::move(n);       \
        }                                \
    }
    P(sabm)
    P(sabme)
    P(ui)
    P(iframe)
    P(dm)
    P(disc)
    P(dl_data)
#define P2(xx)                          \
    void xx()                           \
    {                                   \
        if (auto n = state_->xx(); n) { \
            state_ = std::move(n);      \
        }                               \
    }
    P2(timer1_tick)

    ConnectionData& data() noexcept { return data_; }
    void send_packet(const ax25::Packet& packet);

    void set_src(std::string_view sv) { src_ = sv; }
    void set_dst(std::string_view sv) { dst_ = sv; }

protected:
    std::string src_;
    std::string dst_;
    ConnectionData data_;
    send_func_t send_;
    std::unique_ptr<ConnectionState> state_;
};

} // namespace seqpacket::con

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
#ifndef __INCLUDE__SEQPACKET_CON_H__
#define __INCLUDE__SEQPACKET_CON_H__
/*
 * Replacement state machine for seqpacket.
 */
#include "proto/gen/ax25.pb.h"
#include "util.h"

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
    using time_point_t = std::chrono::time_point<std::chrono::steady_clock>;

    Timer(std::string_view sv) : name_(sv) {}
    void set(int ms) { ms_ = ms; }
    int get() { return ms_; }
    void set_connection_id(int id) { connection_id_ = id; }
    void start()
    {
        ax25ms::log() << connection_id_ << " Starting timer " << name_
                      << " with ms=" << ms_;
        running_ = true;
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds{ ms_ };
    }
    void stop()
    {
        ax25ms::log() << connection_id_ << " Stopping timer " << name_;
        stopped_remaining_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline_ - std::chrono::steady_clock::now())
                                    .count();
        running_ = false;
    }
    int stopped_remaining_ms() const { return std::max(0, stopped_remaining_ms_); }
    void restart()
    {
        stop();
        start();
    }
    bool running() const { return running_; }
    bool expired() const
    {
        return running_ && std::chrono::steady_clock::now() > deadline_;
    }
    time_point_t deadline() const { return deadline_; }

private:
    int connection_id_ = 0;
    int stopped_remaining_ms_ = 0;
    int ms_ = -1;
    time_point_t deadline_;
    bool running_ = false;
    const std::string name_;
};

struct ConnectionData {
    // ax25ms
    int connection_id = 0;

    int modulus = 0;

    // Flags (Page 82)
    bool layer3_initiated = false;
    bool peer_receiver_busy = false;
    bool own_receiver_busy = false;
    bool reject_exception = false;
    bool selective_reject_exception = false;
    bool acknowledge_pending = false;
    double srt = 0;
    int t1v = 0; // Next value for T1; default init value is initial value of SRT.
    unsigned int n1 = 65000; // Max number of octets in the information field of a frame.
    int n2 = 3;              // Max number of retries permitted.

    Timer t1{ "t1" }; // Outstanding I frame or P-bit
    Timer t2{ "t2" }; // Response delay timer. 6.7.1.2
    Timer t3{ "t3" }; // Idle supervision (keep alive).

    int rc{};
    // int nr{};

    // 6.7.2.3. Maximum Number of I Frames Outstanding (k)
    //
    // The maximum number of I frames outstanding at a time is seven
    // (modulo 8) or 127 (modulo 128).
    int k = 7;

    // 4.2.4.1: Send State Variable.
    //
    // The send state variable exists within the TNC and is never
    // sent. It contains the next sequential number to be assigned to
    // the next transmitted I frame. This variable is updated with the
    // transmission of each I frame.
    int vs{};

    // 4.2.4.5. Acknowledge State Variable V(A)
    //
    // The acknowledge state variable exists within the TNC and is
    // never sent. It contains the sequence number of the last frame
    // acknowledged by its peer [V(A)-1 equals the N(S) of the last
    // acknowledged I frame].
    int va{};

    // RC = retry count?

    // 4.2.4.3. Receive State Variable V(R)
    //
    // The receive state variable exists within the TNC. It contains
    // the sequence number of the next expected received I frame. This
    // variable is updated upon the reception of an error-free I frame
    // whose send sequence number equals the present received state
    // variable value
    int vr{};

    bool srej_enabled = false;
    int sreject_exception{};

    // Queues (page 81).
    std::deque<ax25::Packet> iframe_queue_;

    // Resend queue.
    //
    // TODO: there's lots of copying going on. Maybe best to have the
    // resend queue be the canonical store, and have other places carry
    // pointers/iterators?
    //
    // deque iterators are not invalidated by inserts/deletes at the
    // ends.
    std::deque<ax25::Packet> iframe_resend_queue;
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

    virtual std::string name() const { return "base"; }

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
    virtual stateptr_t timer1_tick();
    virtual stateptr_t timer3_tick();

    // Request establishment of AX.25.
    virtual stateptr_t dl_connect(std::string_view dst, std::string_view src);

    // Transmit data using connection oriented protocol.
    virtual stateptr_t dl_data(std::string_view sv);

    // Poll the transmit queue and transmit if appropriate.
    // Call after any other op, in case they cleared to send.
    virtual stateptr_t dl_data_poll();

    // Transmit data using connectionless protocol.
    // virtual stateptr_t dl_unit_data();

    // Release AX.25 connection.
    virtual stateptr_t dl_disconnect();

    virtual bool can_receive_data() const = 0;

    void update_ack(int nr);
    void clear_iframe_queue();

protected:
    // senders.
    void send_sabm(bool poll);
    void send_sabme(bool poll);
    void send_ua(bool poll);
    void send_dm(bool poll);
    void send_rr(bool poll, int nr, bool command);
    void send_rnr(bool poll, int nr);
    void send_rej(bool poll, int nr);
    void send_srej(bool poll, int nr);
    void send_disc(bool poll);

    // subroutines.
    void nr_error_recovery();
    void establish_data_link();
    void clear_exception_conditions();
    void transmit_enquiry();
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
    void dl_error(const DLError& e) const;
};

namespace StateNames {
constexpr const char* Disconnected = "Disconnected";
constexpr const char* Connected = "Connected";
constexpr const char* AwaitingConnection = "AwaitingConnection";
constexpr const char* AwaitingRelease = "AwaitingRelease";
constexpr const char* TimerRecovery = "TimerRecovery";
} // namespace StateNames

class Connection
{
public:
    using send_func_t = std::function<grpc::Status(const ax25::Packet&)>;
    using receive_func_t = std::function<void(std::string_view)>;
    using state_change_t = std::function<void(ConnectionState*)>;

    Connection(int connection_id, send_func_t send, receive_func_t receive);
#define P(xx)                              \
    template <typename T>                  \
    void xx(const T& p)                    \
    {                                      \
        maybe_change_state(state_->xx(p)); \
    }
    P(sabm)
    P(sabme)
    P(ui)
    P(ua)
    P(rr)
    P(iframe)
    P(dm)
    P(disc)
    P(dl_data)
    P(dl_data_poll)
#define P0(xx) \
    void xx() { maybe_change_state(state_->xx()); }
    P0(timer1_tick)
    P0(timer3_tick)
    P0(dl_disconnect)
#define P2(xx)                                \
    template <typename T0, typename T1>       \
    void xx(const T0& a, const T1& b)         \
    {                                         \
        maybe_change_state(state_->xx(a, b)); \
    }
    P2(dl_connect)

    ConnectionData& data() noexcept { return data_; }
    const ConnectionData& data() const noexcept { return data_; }
    void send_packet(const ax25::Packet& packet);

    void set_src(std::string_view sv) { src_ = sv; }
    void set_dst(std::string_view sv) { dst_ = sv; }
    std::string dst() const { return dst_; }
    std::string src() const { return src_; }
    void deliver(const ax25::Packet& p);
    std::string state_name() const { return state_->name(); }
    const ConnectionState& state() const { return *state_; }

    void set_state_change_cb(state_change_t cb) { state_change_ = cb; }

protected:
    void maybe_change_state(std::unique_ptr<ConnectionState>&& st);

    const int connection_id_;

    std::string src_;
    std::string dst_;
    ConnectionData data_;

    state_change_t state_change_; // = []{};
    send_func_t send_;
    receive_func_t receive_;
    std::unique_ptr<ConnectionState> state_;
};

} // namespace seqpacket::con
#endif

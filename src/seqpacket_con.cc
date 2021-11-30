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
#include "seqpacket_con.h"
#include "util.h"

#include "proto/gen/ax25.pb.h"

#include <string_view>
#include <deque>
#include <iostream>
#include <memory>

void log(std::string_view sv) { std::clog << sv << "\n"; }

namespace seqpacket::con {
void ConnectionState::dl_error(const DLError& e)
{
    std::cerr << "DL ERROR: " << int(e) << "\n";
}

ConnectionState::stateptr_t ConnectionState::iframe(const ax25::Packet& p)
{
    throw std::runtime_error("Unexpected iframe in state " + name());
}

ConnectionState::stateptr_t ConnectionState::ua(const ax25::Packet& p)
{
    throw std::runtime_error("Unexpected ua in state " + name());
}
ConnectionState::stateptr_t ConnectionState::dm(const ax25::Packet& p)
{
    throw std::runtime_error("Unexpected dm in state " + name());
}
ConnectionState::stateptr_t ConnectionState::disc(const ax25::Packet& p)
{
    throw std::runtime_error("Unexpected disc in state " + name());
}
ConnectionState::stateptr_t ConnectionState::sabm(const ax25::Packet& p)
{
    throw std::runtime_error("Unexpected sabm in state " + name());
}
ConnectionState::stateptr_t ConnectionState::sabme(const ax25::Packet& p)
{
    throw std::runtime_error("Unexpected sabme in state " + name());
}
ConnectionState::stateptr_t ConnectionState::frmr(const ax25::Packet& p)
{
    throw std::runtime_error("Unexpected frmr in state " + name());
}
ConnectionState::stateptr_t ConnectionState::rr(const ax25::Packet& p)
{
    throw std::runtime_error("Unexpected rr in state " + name());
}
ConnectionState::stateptr_t ConnectionState::ui(const ax25::Packet& p)
{
    throw std::runtime_error("Unexpected ui in state " + name());
}

void ConnectionState::nr_error_recovery()
{
    dl_error(DLError::J);
    establish_data_link();
    d.layer3_initiated = false;
}

// Page 106.
void ConnectionState::establish_data_link()
{
    clear_exception_conditions();
    d.rc = 0;
    send_sabm(true);
    d.t3.stop();
    d.t1.restart();
}

// Page 106.
void ConnectionState::clear_exception_conditions()
{
    d.peer_receiver_busy = false;
    d.reject_exception = false;
    d.own_receiver_busy = false;
    d.acknowledge_pending = false;
}

// Page 106.
void ConnectionState::transmit_enquiry(int rc)
{
    const auto p = true;
    const auto nr = d.vr;

    if (d.own_receiver_busy) {
        send_rnr(p, nr);
    } else {
        send_rr(p, nr);
    }
    d.acknowledge_pending = false;
    d.t1.start();
}

// Page 106.
void ConnectionState::enquiry_response(bool f)
{
    const auto nr = d.vr;
    if (d.own_receiver_busy) {
        send_rnr(f, nr);
    } else {
        send_rr(f, nr);
    }
    d.acknowledge_pending = false;
}

// Page 107.
//
// TODO: The specs have a bug in this section. We should never trigger another
// retransmission before we've at least had a time to do one, and waited one RTT.
//
// Otherwise getting 10 REJs will immediately trigger 10 retransmissions of the
// whole unacked window.
void ConnectionState::invoke_retransmission(int nr)
{
    for (int tmp_vs = nr; tmp_vs != d.vs; tmp_vs = (tmp_vs + 1) % d.modulus) {
        // push old frame tmp_vs on queue
    }
}

// Page 107.
void ConnectionState::check_iframe_acked(int nr)
{
    if (d.peer_receiver_busy) { // Typo in spec. It says "peer busy"
        d.va = nr;
        d.t3.start();
        if (!d.t1.running()) {
            d.t1.start();
        }
        return;
    }

    if (nr == d.vs) {
        d.va = nr;
        d.t1.stop();
        d.t3.start();
        select_t1_value();
        return;
    }

    // No, not all frames ack'd.
    std::clog << "XXXXXXX Not all frames acked\n";
    if (nr != d.va) {
        d.va = nr;
        d.t1.restart();
    }
}

// // Page 108.
void ConnectionState::check_need_for_response(bool command, bool pf)
{
    if (command && pf) {
        enquiry_response(true);
    } else if (!command && pf) {
        dl_error(DLError::A);
    }
}

// Page 108.
void ConnectionState::ui_check(bool command)
{
    if (!command) {
        dl_error(DLError::Q);
        return;
    }

    if (/* info field length <= N1 and content is octet aligned */ true) {
        // dl-unit-data indication
    } else {
        dl_error(DLError::K);
    }
}

// Page 109.
void ConnectionState::select_t1_value()
{
    // TODO: this updates t1 and according to RTT. Temp disable this.
    return;

    double new_srt = d.srt;
    int new_t1_ = d.t1.get();
    if (d.rc) {
        //        int t1_remain = 0; // remaining time on T1 when last stopped
        //        new_srt = (7 / 8 * srt_) + (1 / 8 * t1_) - (1 / 8 * t1_remain);
        //        t1_ = srt_ * 2; // TODO: what?
    }
    d.srt = new_srt;
}

// Page 109.
void ConnectionState::establish_extended_data_link()
{
    clear_exception_conditions();
    // rc <- 0
    // p <- 1
    send_sabme(true);
    d.t3.stop();
    d.t1.restart();
}

// Page 109.
void ConnectionState::set_version_2()
{
    // set half duplex and implicit reject
    d.modulus = 8;
    // n1r <-- 2048
    // kr <- 4
    d.t2.set(3000);
    d.n2 = 10;
}

// Page 109.
void ConnectionState::set_version_2_2()
{
    // set half duplex selective reject
    d.modulus = 128;
    // n1r <-- 2048
    // kr <- 32
    d.t2.set(3000);
    d.n2 = 10;
}

namespace States {


class Disconnected final : public ConnectionState
{
public:
    Disconnected(Connection* connection) : ConnectionState(connection)
    {
        log("State: Disconnected");
    }
    std::string name() override { return "disconnected"; }
    stateptr_t ui(const ax25::Packet& p) override;
    stateptr_t disc(const ax25::Packet& p) override;
    stateptr_t sabm(const ax25::Packet& p) override;
    stateptr_t sabme(const ax25::Packet& p) override { return sabm(p); }

    stateptr_t dl_connect() override;
};

class AwaitingConnection final : public ConnectionState
{
public:
    AwaitingConnection(Connection* con) : ConnectionState(con)
    {
        log("State: Awaiting connection");
    }

    stateptr_t ua(const ax25::Packet& p) override;
};

class AwaitingRelease final : public ConnectionState
{
public:
    AwaitingRelease(Connection* con) : ConnectionState(con)
    {
        log("State: Awaiting release");
    }
};

// Page 92.
class Connected final : public ConnectionState
{
public:
    Connected(Connection* con) : ConnectionState(con) { log("State: Connected"); }
    std::string name() override { return "connected"; }

    stateptr_t ua(const ax25::Packet& p) override;
    stateptr_t dm(const ax25::Packet& p) override;
    stateptr_t frmr(const ax25::Packet& p) override;
    stateptr_t rr(const ax25::Packet& p) override;
    stateptr_t ui(const ax25::Packet& p) override;
    stateptr_t disc(const ax25::Packet& p) override;
    stateptr_t sabm(const ax25::Packet& p) override;
    stateptr_t iframe(const ax25::Packet& p) override;

    stateptr_t timer1_tick() override;

    stateptr_t dl_data(std::string_view sv) override;
    stateptr_t dl_connect() override;
    stateptr_t dl_disconnect() override;
};

class TimerRecovery final : public ConnectionState
{
public:
    TimerRecovery(Connection* con) : ConnectionState(con)
    {
        log("State: Timer recovery");
    }
    std::string name() override { return "timer recovery"; }

    stateptr_t dl_data(std::string_view sv) override;
    stateptr_t iframe(const ax25::Packet& p) override;
    stateptr_t rr(const ax25::Packet& p) override;
    stateptr_t disc(const ax25::Packet& p) override;
};


ConnectionState::stateptr_t TimerRecovery::rr(const ax25::Packet& p)
{
    const auto command = p.command_response();
    const auto poll = p.rr().poll();
    const auto nr = p.rr().nr();
    const auto ns = p.rr().ns();

    d.peer_receiver_busy = false;
    check_need_for_response(command, poll);
    if (d.va <= nr && nr <= ns) {
        check_iframe_acked(nr);
        return std::make_unique<Connected>(connection_);
    }

    nr_error_recovery();
    return std::make_unique<AwaitingConnection>(connection_);
}

ConnectionState::stateptr_t Disconnected::ui(const ax25::Packet& p)
{
    // if (p==1) {send_dm();}
    return nullptr;
}

ConnectionState::stateptr_t Disconnected::disc(const ax25::Packet& p)
{
    // f <- p
    // send_dm();
    return nullptr;
}

ConnectionState::stateptr_t Disconnected::sabm(const ax25::Packet& p)
{
    bool f;
    if (p.has_sabm()) {
        f = p.sabm().poll();
        set_version_2();
    } else {
        f = p.sabme().poll();
        set_version_2_2();
    }

#if 0
    // TODO
    if (!listening_) {
        send_dm(f);
        return;
    }
#endif

    connection_->set_src(p.dst());
    connection_->set_dst(p.src());
    send_ua(f);
    clear_exception_conditions();
    d.vs = 0;
    d.va = 0;
    d.vr = 0;
    std::clog << "TODO: DL-CONNECT\n";
    d.srt = default_srt;
    d.t1v = 2 * d.srt;
    d.t3.start();

    return std::make_unique<Connected>(connection_);
}

// Page 88.
ConnectionState::stateptr_t AwaitingConnection::ua(const ax25::Packet& p)
{
    const auto f = p.ua().poll();
    if (!f) {
        dl_error(DLError::D);
        return nullptr;
    }

    if (d.layer3_initiated) {
        std::clog << "DL-CONNECT confirm\n";
    } else {
        if (d.vs != d.va) {
            d.iframe_queue_.clear();
            std::clog << "DL-CONNECT confirm\n";
        }
    }
    d.t1.stop();
    d.t3.stop();
    d.vs = 0;
    d.va = 0;
    d.vr = 0;
    select_t1_value();
    return std::make_unique<Connected>(connection_);
}

ConnectionState::stateptr_t Connected::ua(const ax25::Packet& p)
{
    // return to awaiting connection
    return nullptr;
}

// Page 95.
ConnectionState::stateptr_t Connected::rr(const ax25::Packet& p)
{
    const auto command = p.command_response();
    const auto pf = p.rr().poll();
    const auto nr = p.rr().nr();
    d.peer_receiver_busy = false;
    check_need_for_response(command, pf);
    if (d.va < nr && nr < d.vs) {
        check_iframe_acked(nr);
        return std::make_unique<Connected>(connection_);
    }
    nr_error_recovery();
    return std::make_unique<AwaitingConnection>(connection_);
}

ConnectionState::stateptr_t Connected::dm(const ax25::Packet& p)
{
    return std::make_unique<Disconnected>(connection_);
}

ConnectionState::stateptr_t Connected::frmr(const ax25::Packet& p)
{
    dl_error(DLError::K);
    establish_data_link(); // Typo in spec.
    d.layer3_initiated = false;
    return std::make_unique<AwaitingConnection>(connection_);
}

// Page 94.
ConnectionState::stateptr_t Connected::ui(const ax25::Packet& p)
{
    ui_check(p.command_response());
    if (p.ui().push()) {
        enquiry_response(true);
    }
    return nullptr;
}

ConnectionState::stateptr_t TimerRecovery::disc(const ax25::Packet& p)
{
    return connected_timer_recovery_disc(p);
}

// Page 102.
ConnectionState::stateptr_t TimerRecovery::iframe(const ax25::Packet& p)
{
    const auto nr = p.iframe().nr();
    const auto ns = p.iframe().ns();
    const auto poll = p.iframe().poll();
    if (p.command_response()) {
        dl_error(DLError::S);
        // discard frame (implicit)
        return nullptr;
    }
    if (p.iframe().payload().size() > d.n1) {
        dl_error(DLError::O);
        establish_data_link();
        d.layer3_initiated = false;
        return std::make_unique<AwaitingConnection>(connection_);
    }
    if (!(d.va <= nr && nr <= d.vs)) {
        nr_error_recovery();
        return std::make_unique<AwaitingConnection>(connection_);
    }
    d.va = nr;
    if (d.own_receiver_busy) {
        // discard (implicit)
        if (poll) {
            send_rnr(true, d.vr); // TODO: "expidited"
            d.acknowledge_pending = false;
        }
        return nullptr;
    }

    if (ns == d.vr) {
        d.vr++;
        // clear reject exception
        // decrement sreject exception if >0
        std::cout << "DL-DATA INDICATION\n";
        while (/*i frame stored*/ false) {
            //   retrieve stored V(r) i frame
            //   DL-DATA indication
            d.vr++;
        }
        if (poll) {
            send_rr(true, d.vr);
            d.acknowledge_pending = false;
            return nullptr;
        }
        if (!d.acknowledge_pending) {
            // LM seize request.
            d.acknowledge_pending = true;
        }
        return nullptr;
    }

    if (d.reject_exception) {
        // discard iframe (implicit).
        if (!poll) {
            return nullptr;
        }
        send_rr(true, d.vr);
        d.acknowledge_pending = false;
        return nullptr;
    }

    if (!d.srej_enabled) {
        goto tail_srej_off;
    }
    // TODO: save contents of iframe
    if (d.sreject_exception > 0) {
        d.sreject_exception++;
        send_srej(false, ns);
        d.acknowledge_pending = false;
        return nullptr;
    }
    if (ns > d.vr + 1) {
        goto tail_srej_off;
    }
    d.sreject_exception++;
    send_srej(false, d.vr);
    d.acknowledge_pending = false;
    return nullptr;

tail_srej_off:
    // discard iframe (implicit)
    d.reject_exception = true;
    send_rej(poll, d.vr);
    d.acknowledge_pending = false;
    return nullptr;
}

ConnectionState::stateptr_t Connected::disc(const ax25::Packet& p)
{
    return connected_timer_recovery_disc(p);
}


ConnectionState::stateptr_t Connected::sabm(const ax25::Packet& p)
{
    bool f;
    if (p.has_sabm()) {
        f = p.sabm().poll();
    } else if (p.has_sabme()) {
        f = p.sabme().poll();
    } else {
        throw std::runtime_error("internal error: sabm but not sabm");
    }

    send_ua(f);
    clear_exception_conditions();
    dl_error(DLError::F);
    if (d.vs != d.va) {
        d.iframe_queue_.clear();
        // TODO: dl_connect indication
    }
    d.t1.stop();
    d.t3.start();
    d.vs = 0;
    d.va = 0;
    d.vr = 0;
    return nullptr;
}

// Page 97.
ConnectionState::stateptr_t Connected::iframe(const ax25::Packet& p)
{
    const auto nr = p.iframe().nr();
    const auto poll = p.iframe().poll();
    if (!p.command_response()) {
        dl_error(DLError::S);
        // discard frame (implicit).
        return nullptr;
    }
    if (p.iframe().payload().size() > d.n1) {
        dl_error(DLError::O);
        establish_data_link();
        d.layer3_initiated = false;
        return std::make_unique<AwaitingConnection>(connection_);
    }
    if (!(d.va <= nr && nr <= d.vs)) {
        nr_error_recovery();
        return std::make_unique<AwaitingConnection>(connection_);
    }
    check_iframe_acked(nr);
    if (d.own_receiver_busy) {
        // discard (implicit)
        if (poll) {
            send_rnr(true, d.vr);
            d.acknowledge_pending = false;
        }
        return nullptr;
    }

    if (p.iframe().ns() == d.vr) {
        d.vr++;
        // clear reject exception
        // decrement sreject exception if >0
        std::cout << "DL-DATA INDICATION\n";
        while (/*i frame stored*/ false) {
            //   retrieve stored V(r) i frame
            //   DL-DATA indication
            d.vr++;
        }
        if (poll) {
            send_rr(true, d.vr);
            d.acknowledge_pending = false;
            return nullptr;
        }
        if (!d.acknowledge_pending) {
            // LM seize request.
            d.acknowledge_pending = true;
        }
        return nullptr;
    }

    if (d.reject_exception) {
        // discard iframe (implicit).
        if (!poll) {
            return nullptr;
        }
        send_rr(true, d.vr);
        d.acknowledge_pending = false;
        return nullptr;
    }


    return nullptr;
}

// Page 85.
ConnectionState::stateptr_t Disconnected::dl_connect()
{
    // TODO.
    connection_->set_src("M0THC-2");
    connection_->set_dst("M0THC-1");
    d.srt = default_srt; // Spec typo: says SAT
    d.t1v = 2 * d.srt;
    establish_data_link();
    d.layer3_initiated = true;
    return std::make_unique<AwaitingConnection>(connection_);
}

// 85.
ConnectionState::stateptr_t Connected::dl_connect()
{
    d.srt = default_srt; // Spec typo: says SAT
    d.t1v = 2 * d.srt;
    return std::make_unique<AwaitingConnection>(connection_);
}

// Page 93.
ConnectionState::stateptr_t Connected::timer1_tick()
{
    transmit_enquiry(1);
    return std::make_unique<TimerRecovery>(connection_);
}

ConnectionState::stateptr_t Connected::dl_data(std::string_view sv)
{
    ax25::Packet p;
    auto& iframe = *p.mutable_iframe();
    iframe.set_pid(0xF0);
    iframe.set_payload(sv.data(), sv.size());
    d.iframe_queue_.push_back(std::move(p));

    iframe_pop();
    return nullptr;
}

ConnectionState::stateptr_t TimerRecovery::dl_data(std::string_view sv)
{
    // TODO: merge with Connected::dl_data()
    ax25::Packet p;
    auto& iframe = *p.mutable_iframe();
    iframe.set_pid(0xF0);
    iframe.set_payload(sv.data(), sv.size());
    d.iframe_queue_.push_back(std::move(p));

    iframe_pop();
    return nullptr;
}

ConnectionState::stateptr_t Connected::dl_disconnect()
{
    // discard i frame queue
    // rc <- 0
    // send_disc(P=1)
    // step T3
    // start T1
    return std::make_unique<AwaitingRelease>(connection_);
}
} // namespace States

void ConnectionState::send_ua(bool poll)
{
    ax25::Packet packet;
    auto& ua = *packet.mutable_ua();
    ua.set_poll(poll);
    connection_->send_packet(packet);
}

void ConnectionState::send_sabm(bool poll)
{
    ax25::Packet packet;
    auto& sabm = *packet.mutable_sabm();
    sabm.set_poll(poll);
    connection_->send_packet(packet);
}

void ConnectionState::send_sabme(bool poll)
{
    ax25::Packet packet;
    auto& sabme = *packet.mutable_sabme();
    sabme.set_poll(poll);
    connection_->send_packet(packet);
}

void ConnectionState::send_rr(bool poll, int nr)
{
    ax25::Packet packet;
    auto& rr = *packet.mutable_rr();
    rr.set_poll(poll);
    rr.set_nr(nr);
    connection_->send_packet(packet);
}

void ConnectionState::send_rej(bool poll, int nr)
{
    ax25::Packet packet;
    auto& rej = *packet.mutable_rej();
    rej.set_poll(poll);
    rej.set_nr(nr);
    connection_->send_packet(packet);
}

void ConnectionState::send_srej(bool poll, int nr)
{
    ax25::Packet packet;
    auto& srej = *packet.mutable_srej();
    srej.set_poll(poll);
    srej.set_nr(nr);
    connection_->send_packet(packet);
}

void ConnectionState::send_rnr(bool poll, int nr)
{
    ax25::Packet packet;
    auto& rnr = *packet.mutable_rnr();
    rnr.set_poll(poll);
    rnr.set_nr(nr);
    connection_->send_packet(packet);
}

Connection::Connection(send_func_t send)
    : send_(send), state_(std::make_unique<States::Disconnected>(this))
{
}

ConnectionState::ConnectionState(Connection* connection)
    : d(connection->data()), connection_(connection)
{
}

void ConnectionState::iframe_pop()
{
    if (d.peer_receiver_busy) {
        // push frame on queue
        return;
    }

    // Check window full.
    if (d.vs == (d.va + d.k) % d.modulus) {
        // push i frame on queue
        return;
    }

    const auto ns = d.vs;
    const auto nr = d.vr;
    auto packet = d.iframe_queue_.front();
    d.iframe_queue_.pop_front();

    auto& iframe = *packet.mutable_iframe();
    iframe.set_ns(ns);
    iframe.set_nr(nr);

    connection_->send_packet(packet);

    d.vs = (d.vs + 1) % d.modulus;

    if (!d.t1.running()) {
        d.t1.start();
        d.t3.stop();
    }
}

ConnectionState::stateptr_t
ConnectionState::connected_timer_recovery_disc(const ax25::Packet& p)
{
    d.iframe_queue_.clear();
    const auto f = p.disc().poll();
    send_ua(f);
    std::cout << "DL-DISCONNECT indication\n";
    d.t1.stop();
    d.t3.stop();
    return std::make_unique<States::Disconnected>(connection_);
}

void Connection::send_packet(const ax25::Packet& packet)
{
    auto p = packet;
    p.set_src(src_);
    p.set_dst(dst_);
    send_(p);
}

} // namespace seqpacket::con
int wrapmain() { return 0; }

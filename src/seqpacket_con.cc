/*
   Copyright 2021-2022 Google LLC

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
 * State machine for seqpacket.
 *
 * Unimplemented (known so far):
 * * SREJ
 * * REJ is untested.
 * * A fix for the RR DoS
 */
#include "seqpacket_con.h"
#include "util.h"

#include "proto/gen/ax25.pb.h"

#include <string_view>
#include <deque>
#include <iostream>
#include <memory>

using ax25ms::log;

namespace {
bool in_range(int va, int nr, int vs, int mod)
{
    for (int t = va;; t = (t + 1) % mod) {
        if (t == nr) {
            return true;
        }
        if (t == vs) {
            return false;
        }
    }
}
} // namespace

namespace seqpacket::con {
void ConnectionState::dl_error(const DLError& e) const
{
    // Page 81.
    std::map<DLError, std::string> sm = {
        { DLError::A, "A: F=1 received but P=1 not outstanding" },
        { DLError::B, "B: Unexpected DM with F=1 in states 3,4,5" },
        { DLError::C, "C: Unexpected UA in states 3,4,5" },
        { DLError::D, "D: UA received without F=1 when SABM or DISC was sent P=1" },
        { DLError::E, "E: DM received in states 3,4,5" },
        { DLError::F, "F: Data link reset; i.e., SABM received instate 3,4,5" },
        { DLError::G, "G: Connection timed out" }, // TODO: specs don't list this.
        //{DLError::H, ""},
        { DLError::I, "I: N2 timeouts; unacknowledged data" },
        { DLError::J, "J: N(r) sequence error" },
        //{DLError::K, ""},
        { DLError::L, "L: Control field invalid or not implemented" },
        { DLError::M, "M: Information field was received in a U- or S-type frame" },
        { DLError::N, "N: Length of frame incorrect for frame type" },
        { DLError::O, "O: I frame exceeded maximum allowed length" },
        { DLError::P, "P: N(s) out of the window" },
        { DLError::Q, "Q: UI response received, or UI command with P=1 received" },
        { DLError::R, "R: UI frame exceeded maximum allowed length" },
        { DLError::S, "S: I response received" },
        { DLError::T, "T: N2 timeout; no response to enquiry" },
        { DLError::U, "U: N2 timeouts; extended pere busy condition" },
        { DLError::V, "V: No DL machines available to establish connection" },
    };
    std::string s;
    const auto itr = sm.find(e);
    if (itr == sm.end()) {
        s = "unknown";
    } else {
        s = itr->second;
    }
    log() << d.connection_id << " DL ERROR: " << int(e) << " (" << s << ")";
}

/*
 * Default state callback handlers.
 *
 * Some throw, because the connection should die. Others are just
 * warnings, maybe because handler is just not implemented.
 */

ConnectionState::stateptr_t ConnectionState::iframe(const ax25::Packet& p)
{
    log() << d.connection_id << " ERROR: unhandled iframe in state " << name();
    return nullptr;
}

ConnectionState::stateptr_t ConnectionState::ua(const ax25::Packet& p)
{
    log() << d.connection_id << " ERROR: unhandled UA in state " << name();
    return nullptr;
}
ConnectionState::stateptr_t ConnectionState::dm(const ax25::Packet& p)
{
    log() << d.connection_id << " ERROR: unhandled DM in state " << name();
    return nullptr;
}
ConnectionState::stateptr_t ConnectionState::disc(const ax25::Packet& p)
{
    log() << d.connection_id << " ERROR: unhandled DISC in state " << name();
    return nullptr;
}
ConnectionState::stateptr_t ConnectionState::sabm(const ax25::Packet& p)
{
    log() << d.connection_id << " ERROR: unhandled SABM in state " << name();
    return nullptr;
}
ConnectionState::stateptr_t ConnectionState::sabme(const ax25::Packet& p)
{
    log() << d.connection_id << " ERROR: unhandled SABME in state " << name();
    return nullptr;
}
ConnectionState::stateptr_t ConnectionState::frmr(const ax25::Packet& p)
{
    log() << d.connection_id << " ERROR: unhandled FRMR in state " << name();
    return nullptr;
}
ConnectionState::stateptr_t ConnectionState::rr(const ax25::Packet& p)
{
    log() << d.connection_id << " ERROR: unhandled RR in state " << name();
    return nullptr;
}
ConnectionState::stateptr_t ConnectionState::ui(const ax25::Packet& p)
{
    log() << d.connection_id << " ERROR: unhandled UI in state " << name();
    return nullptr;
}

ConnectionState::stateptr_t ConnectionState::dl_data(std::string_view sv)
{
    log() << d.connection_id << " ERROR: unhandled dl_data in state " << name();
    return nullptr;
}

ConnectionState::stateptr_t ConnectionState::timer1_tick()
{
    log() << d.connection_id << " ERROR: unhandled T1 in state " << name();
    return nullptr;
}

ConnectionState::stateptr_t ConnectionState::timer3_tick()
{
    log() << d.connection_id << " ERROR: unhandled T3 in state " << name();
    return nullptr;
}

ConnectionState::stateptr_t ConnectionState::dl_connect(std::string_view dst,
                                                        std::string_view src)
{
    log() << d.connection_id << " ERROR: unexpected dl_connect in state " << name();
    return nullptr;
}

ConnectionState::stateptr_t ConnectionState::dl_disconnect()
{
    log() << d.connection_id << " ERROR: unexpected dl_disconnect in state " << name();
    return nullptr;
}

ConnectionState::stateptr_t ConnectionState::dl_data_poll() { return nullptr; }


// Page 106.
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
void ConnectionState::transmit_enquiry()
{
    const auto p = true;
    const auto nr = d.vr;

    if (d.own_receiver_busy) {
        send_rnr(p, nr);
    } else {
        send_rr(p, nr, true);
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
        send_rr(f, nr, false); // Bug in spec. Spec says to use "RR command".
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
//
// Malicious or duplicated RRs could also trigger it.
void ConnectionState::invoke_retransmission(int nr)
{
    const int x = d.vs;
    std::deque<ax25::Packet> resend;

    d.vs = nr;
    log() << d.connection_id << " Retransmitting from " << nr << " to " << x << " with "
          << d.iframe_resend_queue.size();
    auto itr = d.iframe_resend_queue.rbegin();
    for (int i = nr; i != x; i = (i + 1) % d.modulus) {
        // push old frame with seq d.vs on d.send_queue_
        assert(itr != d.iframe_resend_queue.rend());
        d.iframe_queue_.push_front(*itr++);
    }
    // TODO: the above isn't really creating the right output, so
    // let's just overwrite it. But let's sync it with the specs.
    d.iframe_queue_ = d.iframe_resend_queue;
}

void ConnectionState::update_ack(int nr)
{
    // log() << d.connection_id << " YYY acking to " << nr;
    while (d.va != nr) {
        assert(!d.iframe_resend_queue.empty());
        d.iframe_resend_queue.pop_front();
        d.va = (d.va + 1) % d.modulus;
    }
}

// Page 107.
void ConnectionState::check_iframe_acked(int nr)
{
    if (d.peer_receiver_busy) { // Typo in spec. It says "peer busy"
        update_ack(nr);
        d.t3.start();
        if (!d.t1.running()) {
            d.t1.start();
        }
        return;
    }

    if (nr == d.vs) {
        update_ack(nr);
        d.t1.stop();
        d.t3.start();
        select_t1_value();
        return;
    }

    // No, not all frames ack'd.
    log() << d.connection_id << " XXXXXXX Not all frames acked";
    if (nr != d.va) {
        update_ack(nr);
        d.t1.restart();
    }
}

// Page 108.
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
        log() << d.connection_id << " DL-UNIT-DATA indication";
    } else {
        dl_error(DLError::K);
    }
}

// Page 109.
/*
 * select_t1_value() chooses the new T1 timer value.
 *
 * T1 is how long to wait before retransmitting an unacknowledged
 * frame.
 *
 * The code currently uses the algorithm in the specs, except with
 * min/max clamping to 100ms-10s.
 *
 * Linux has three modes, depending on what AX25_BACKOFF is set to.
 *   0. None.
 *   1. Linear.
 *   2. Exponential.
 * Where linear (1) is default.
 * See kernel source net/ax25/ax25_subr.c:ax25_calculate_t1
 */
void ConnectionState::select_t1_value()
{
    if (false) {
        // TODO: this updates t1 and according to RTT. Temp disable this.
        d.srt = 1500;
        d.t1.set(d.srt * 2);
        return;
    }

    int min_srt = 100;
    int max_srt = 10000;

    auto clamp = [min_srt, max_srt](int in) {
        return std::max(min_srt, std::min(max_srt, in));
    };

    if (d.rc == 0) {
        log() << "Adjusting rc=0 t1=" << d.t1.get() << " srt=" << d.srt
              << " remain=" << d.t1.stopped_remaining_ms();
        d.srt = clamp(7.0 / 8.0 * d.srt +
                      1.0 / 8.0 * (d.t1.get() - d.t1.stopped_remaining_ms()));
        // TODO: should this be old srt or new?
        d.t1.set(clamp(d.srt * 2));
        log() << "New values: t1=" << d.t1.get() << " srt=" << d.srt;
    } else if (d.t1.expired()) {
        log() << "Adjusting t1 expired t1=" << d.t1.get() << " srt=" << d.srt;

        // TODO: should it be *2 again? Seems excessive.
        d.t1.set(clamp((2 << d.rc) * d.srt));
        log() << "New values: t1=" << d.t1.get() << " srt=" << d.srt;
    }
}

// Page 109.
void ConnectionState::establish_extended_data_link()
{
    clear_exception_conditions();
    d.rc = 0;
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

class ConnectedTimerRecovery : public ConnectionState
{
public:
    ConnectedTimerRecovery(Connection* con) : ConnectionState(con) {}
    stateptr_t dl_disconnect() override;
    stateptr_t dl_data(std::string_view sv) override;
    stateptr_t dl_data_poll() override;
    stateptr_t disc(const ax25::Packet& p) override;
    stateptr_t ua(const ax25::Packet& p) override;
    stateptr_t dm(const ax25::Packet& p) override;
    stateptr_t iframe(const ax25::Packet& p) override;

    bool can_receive_data() const override { return true; }
};

// Page 92.
class Connected final : public ConnectedTimerRecovery
{
public:
    Connected(Connection* con) : ConnectedTimerRecovery(con) {}
    std::string name() const override { return StateNames::Connected; }

    stateptr_t frmr(const ax25::Packet& p) override;
    stateptr_t rr(const ax25::Packet& p) override;
    stateptr_t ui(const ax25::Packet& p) override;
    stateptr_t sabm(const ax25::Packet& p) override;
    stateptr_t timer1_tick() override;
    stateptr_t dl_connect(std::string_view dst, std::string_view src) override;
};

class TimerRecovery final : public ConnectedTimerRecovery
{
public:
    TimerRecovery(Connection* con) : ConnectedTimerRecovery(con) {}
    std::string name() const override { return StateNames::TimerRecovery; }

    stateptr_t rr(const ax25::Packet& p) override;
    stateptr_t timer1_tick() override;
};

class Disconnected final : public ConnectionState
{
public:
    Disconnected(Connection* connection) : ConnectionState(connection) {}
    std::string name() const override { return StateNames::Disconnected; }
    stateptr_t ui(const ax25::Packet& p) override;
    stateptr_t disc(const ax25::Packet& p) override;
    stateptr_t sabm(const ax25::Packet& p) override;
    stateptr_t sabme(const ax25::Packet& p) override { return sabm(p); }

    stateptr_t dl_connect(std::string_view dst, std::string_view src) override;

    bool can_receive_data() const override { return false; }
};

class AwaitingConnection final : public ConnectionState
{
public:
    AwaitingConnection(Connection* con) : ConnectionState(con) {}
    std::string name() const override { return StateNames::AwaitingConnection; }

    stateptr_t ua(const ax25::Packet& p) override;
    stateptr_t timer1_tick() override;
    bool can_receive_data() const override { return false; }
};

class AwaitingRelease final : public ConnectionState
{
public:
    AwaitingRelease(Connection* con) : ConnectionState(con) {}
    bool can_receive_data() const override { return false; }
};

// Page 99.
ConnectionState::stateptr_t TimerRecovery::rr(const ax25::Packet& p)
{
    const auto command = p.command_response();
    const auto pf = p.rr().poll();
    const auto nr = p.rr().nr();

    d.peer_receiver_busy = false;
    if (!command && pf) {
        d.t1.stop();
        select_t1_value();
        // log() << d.connection_id << " XXX Va nr Vs " << d.va << nr << d.vs;
        if (!in_range(d.va, nr, d.vs, d.modulus)) {
            nr_error_recovery();
            return std::make_unique<AwaitingConnection>(connection_);
        }
        update_ack(nr);
        // log() << d.connection_id << " XXX Vs Va " << d.vs << d.va;
        if (d.vs != d.va) {
            invoke_retransmission(nr);
            return nullptr;
        }
        d.t3.start();
        return std::make_unique<Connected>(connection_);
    }

    if (command && pf) {
        enquiry_response(true);
    }
    if (!in_range(d.va, nr, d.vs, d.modulus)) {
        nr_error_recovery();
        return std::make_unique<AwaitingConnection>(connection_);
    }
    update_ack(nr);
    return nullptr;
}

// Page 84.
ConnectionState::stateptr_t Disconnected::ui(const ax25::Packet& p)
{
    const auto pf = p.ui().push();
    const auto command = p.command_response();

    ui_check(command);
    if (pf) {
        send_dm(true);
    }
    return nullptr;
}

// Page 84.
ConnectionState::stateptr_t Disconnected::disc(const ax25::Packet& p)
{
    const auto pf = p.disc().poll();

    send_dm(pf);
    return nullptr;
}

// Page 85.
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
        return nullptr;
    }
#endif

    connection_->set_src(p.dst());
    connection_->set_dst(p.src());
    send_ua(f);
    clear_exception_conditions();
    d.vs = 0;
    d.va = 0;
    d.vr = 0;
    log() << d.connection_id << " TODO: DL-CONNECT";
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
        log() << d.connection_id << " DL-CONNECT confirm";
    } else {
        if (d.vs != d.va) {
            clear_iframe_queue();
            log() << d.connection_id << " DL-CONNECT indication";
        }
    }
    // log() << d.connection_id << " AwaitingConnection t1 stop";
    d.t1.stop();
    d.t3.stop();
    d.vs = 0;
    d.va = 0;
    d.vr = 0;
    select_t1_value();
    return std::make_unique<Connected>(connection_);
}

// Page 93 100.
ConnectionState::stateptr_t ConnectedTimerRecovery::ua(const ax25::Packet& p)
{
    dl_error(DLError::C);
    establish_data_link();
    d.layer3_initiated = false;
    return std::make_unique<AwaitingConnection>(connection_);
}

// Page 95.
ConnectionState::stateptr_t Connected::rr(const ax25::Packet& p)
{
    const auto command = p.command_response();
    const auto pf = p.rr().poll();
    const auto nr = p.rr().nr();

    d.peer_receiver_busy = false;
    check_need_for_response(command, pf);
    if (!in_range(d.va, nr, d.vs, d.modulus)) {
        nr_error_recovery();
        return std::make_unique<AwaitingConnection>(connection_);
    }
    check_iframe_acked(nr);
    return nullptr;
}

// Page 93 & 101.
ConnectionState::stateptr_t ConnectedTimerRecovery::dm(const ax25::Packet& p)
{
    dl_error(DLError::E);
    log() << d.connection_id << " DL-DISCONNECT indication";
    clear_iframe_queue();
    // log() << d.connection_id << " Connected::dm t1 stop";
    d.t1.stop();
    d.t3.stop();
    return std::make_unique<Disconnected>(connection_);
}

// Page 94.
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
    const auto pf = p.ui().push();
    const auto command = p.command_response();

    ui_check(command);
    if (pf) {
        enquiry_response(true);
    }
    return nullptr;
}

// Page 96 & 102.
ConnectionState::stateptr_t ConnectedTimerRecovery::iframe(const ax25::Packet& p)
{
    const auto nr = p.iframe().nr();
    const auto ns = p.iframe().ns();
    const auto poll = p.iframe().poll();
    const auto command = p.command_response();
    if (!command) {
        // TODO: do we really care?
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
    if (!in_range(d.va, nr, d.vs, d.modulus)) {
        nr_error_recovery();
        return std::make_unique<AwaitingConnection>(connection_);
    }
    if (name() == "Connected") {
        check_iframe_acked(nr);
    } else {
        update_ack(nr); // Different from Connected.
        assert(name() == "TimerRecovery");
    }
    if (d.own_receiver_busy) {
        // discard (implicit)
        if (poll) {
            send_rnr(true, d.vr); // TODO: If TimerRecovery then "expidited"
            d.acknowledge_pending = false;
        }
        return nullptr;
    }

    if (ns == d.vr) {
        d.vr = (d.vr + 1) % d.modulus;
        // clear reject exception
        // decrement sreject exception if >0
        log() << d.connection_id << " DL-DATA INDICATION";
        connection_->deliver(p);

        // TODO: Check for any out of order iframes unlocking
        // previously received packets.
        while (/*i frame stored*/ false) {
            //   retrieve stored V(r) i frame
            //   DL-DATA indication
            d.vr = (d.vr + 1) % d.modulus;
        }

        if (poll) {
            send_rr(true, d.vr, false);
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
        if (poll) {
            send_rr(true, d.vr, false);
            d.acknowledge_pending = false;
        }
        return nullptr;
    }

    auto tail_srej_off = [this, poll] {
        // discard iframe (implicit)
        d.reject_exception = true;
        send_rej(poll, d.vr);
        d.acknowledge_pending = false;
        return nullptr;
    };

    if (!d.srej_enabled) {
        return tail_srej_off();
    }

    // TODO: save contents of iframe

    if (d.sreject_exception > 0) {
        d.sreject_exception++;
        send_srej(false, ns);
        d.acknowledge_pending = false;
        return nullptr;
    }

    // TODO: Original says "ns > vr + 1", but this is correct with mod
    // arithmetic?
    //
    // But this means we only accept one packet out of order?
    if (ns != d.vr + 1) {
        return tail_srej_off();
    }

    d.sreject_exception++;
    send_srej(false, d.vr); // Or should it be true? F=1
    d.acknowledge_pending = false;
    return nullptr;
}

// Page 93 & 100.
ConnectionState::stateptr_t ConnectedTimerRecovery::disc(const ax25::Packet& p)
{
    clear_iframe_queue();
    const auto f = p.disc().poll();
    send_ua(f);
    log() << d.connection_id << " DL-DISCONNECT indication";
    d.t1.stop();
    d.t3.stop();
    return std::make_unique<States::Disconnected>(connection_);
}

// Page 93.
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
        clear_iframe_queue();
        log() << d.connection_id << " DL-CONNECT indication";
    }
    // log() << d.connection_id << " Connected::sabm t1 stop";
    d.t1.stop();
    d.t3.start();
    d.vs = 0;
    d.va = 0;
    d.vr = 0;
    return nullptr;
}

// Page 85.
ConnectionState::stateptr_t Disconnected::dl_connect(std::string_view dst,
                                                     std::string_view src)
{
    d.modulus = 8; // TODO
    connection_->set_src(src);
    connection_->set_dst(dst);
    d.srt = default_srt; // Spec typo: says SAT
    d.t1v = 2 * d.srt;
    d.t1.set(d.t1v);
    establish_data_link();
    d.layer3_initiated = true;
    return std::make_unique<AwaitingConnection>(connection_);
}

// Page 92.
ConnectionState::stateptr_t Connected::dl_connect(std::string_view dst,
                                                  std::string_view src)
{
    connection_->set_src(src);
    connection_->set_dst(dst);
    clear_iframe_queue();
    establish_data_link();
    d.layer3_initiated = true;
    return std::make_unique<AwaitingConnection>(connection_);
}

// Page 93.
ConnectionState::stateptr_t Connected::timer1_tick()
{
    log() << d.connection_id << " Connected::timer1_tick()";
    d.rc = 1;
    transmit_enquiry();
    return std::make_unique<TimerRecovery>(connection_);
}

// Page 99.
ConnectionState::stateptr_t TimerRecovery::timer1_tick()
{
    log() << d.connection_id << " TimerRecovery::timer1_tick()";
    if (d.rc != d.n2) {
        d.rc++;
        transmit_enquiry();
        return nullptr;
    }

    if (d.va != d.vs) {
        dl_error(DLError::I);
    } else if (d.peer_receiver_busy) { // Typo in spec: "peer busy"
        dl_error(DLError::U);
    } else {
        dl_error(DLError::T);
    }
    log() << d.connection_id << " TODO: DL-Disconnect request?";
    return std::make_unique<Disconnected>(connection_);
}

// Page 88.
ConnectionState::stateptr_t AwaitingConnection::timer1_tick()
{
    if (d.rc == d.n2) {
        clear_iframe_queue();
        dl_error(DLError::G);
        log() << d.connection_id << " DL-DISCONNECT indication";
        return std::make_unique<States::Disconnected>(connection_);
    }
    d.rc++;
    send_sabm(true);
    select_t1_value();
    d.t1.start();
    return nullptr;
}

// Page 92 & 98.
ConnectionState::stateptr_t ConnectedTimerRecovery::dl_data_poll()
{
    while (!d.iframe_queue_.empty()) {
        // log() << d.connection_id << " flush?";
        // Receiver busy.
        if (d.peer_receiver_busy) {
            // "Push i frame on queue", here meaning leave it on the
            // queue.
            // log() << d.connection_id << " no, receiver busy";
            break;
        }

        // Check window full.
        if (d.vs == (d.va + d.k) % d.modulus) {
            // Same.
            // log() << d.connection_id << " no, window is closed vs va k " << d.vs
            // << d.va << d.k;
            break;
        }

        const auto ns = d.vs;
        const auto nr = d.vr;
        auto packet = d.iframe_queue_.front();
        d.iframe_queue_.pop_front();

        auto& iframe = *packet.mutable_iframe();
        iframe.set_ns(ns);
        iframe.set_nr(nr);

        // OUT OF SPEC:
        // If this is the last packet in the window then set the poll bit.
        if (d.vs == (d.va + d.k + d.modulus - 1) % d.modulus) {
            iframe.set_poll(true);
        }

        connection_->send_packet(packet);

        d.vs = (d.vs + 1) % d.modulus;

        if (!d.t1.running()) {
            // log() << d.connection_id << " >>>>>>> t1 start from sending";
            d.t1.start();
            d.t3.stop();
        }
    }
    return nullptr;
}

// Page 92 & 98.
ConnectionState::stateptr_t ConnectedTimerRecovery::dl_data(std::string_view sv)
{
    ax25::Packet p;
    auto& iframe = *p.mutable_iframe();
    iframe.set_pid(0xf0);
    iframe.set_payload(sv.data(), sv.size());
    p.set_command_response(true);
    d.iframe_resend_queue.push_back(p);
    d.iframe_queue_.push_back(std::move(p));
    return nullptr;
}

// Page 92 & 98.
ConnectionState::stateptr_t ConnectedTimerRecovery::dl_disconnect()
{
    clear_iframe_queue();
    d.rc = 0;
    send_disc(true);
    d.t3.stop();
    d.t1.start();
    return std::make_unique<AwaitingRelease>(connection_);
}
} // namespace States

void ConnectionState::clear_iframe_queue()
{
    d.iframe_queue_.clear();
    d.iframe_resend_queue.clear();
}

void ConnectionState::send_ua(bool pf)
{
    ax25::Packet packet;
    auto& ua = *packet.mutable_ua();
    ua.set_poll(pf);
    connection_->send_packet(packet);
}

void ConnectionState::send_sabm(bool poll)
{
    ax25::Packet packet;
    auto& sabm = *packet.mutable_sabm();
    sabm.set_poll(poll);
    connection_->send_packet(packet);
}

void ConnectionState::send_dm(bool pf)
{
    ax25::Packet packet;
    auto& dm = *packet.mutable_dm();
    dm.set_poll(pf);
    connection_->send_packet(packet);
}

void ConnectionState::send_disc(bool poll)
{
    ax25::Packet packet;
    auto& disc = *packet.mutable_disc();
    disc.set_poll(poll);
    connection_->send_packet(packet);
}

void ConnectionState::send_sabme(bool poll)
{
    ax25::Packet packet;
    auto& sabme = *packet.mutable_sabme();
    sabme.set_poll(poll);
    connection_->send_packet(packet);
}

void ConnectionState::send_rr(bool pf, int nr, bool command)
{
    ax25::Packet packet;
    packet.set_command_response(command);
    auto& rr = *packet.mutable_rr();
    rr.set_poll(pf);
    rr.set_nr(nr);
    connection_->send_packet(packet);
}

void ConnectionState::send_rej(bool pf, int nr)
{
    ax25::Packet packet;
    auto& rej = *packet.mutable_rej();
    rej.set_poll(pf);
    rej.set_nr(nr);
    connection_->send_packet(packet);
}

void ConnectionState::send_srej(bool pf, int nr)
{
    ax25::Packet packet;
    auto& srej = *packet.mutable_srej();
    srej.set_poll(pf);
    srej.set_nr(nr);
    connection_->send_packet(packet);
}

void ConnectionState::send_rnr(bool pf, int nr)
{
    ax25::Packet packet;
    auto& rnr = *packet.mutable_rnr();
    packet.set_command_response(true);
    rnr.set_poll(pf);
    rnr.set_nr(nr);
    connection_->send_packet(packet);
}

Connection::Connection(int connection_id, send_func_t send, receive_func_t receive)
    : connection_id_(connection_id),
      send_(send),
      receive_(receive),
      state_(std::make_unique<States::Disconnected>(this))
{
    data_.connection_id = connection_id;
    data_.t1.set(3000);
    data_.t2.set(3000);
    data_.t3.set(300 * 1000);
    data_.t1.set_connection_id(connection_id);
    data_.t2.set_connection_id(connection_id);
    data_.t3.set_connection_id(connection_id);
}

ConnectionState::ConnectionState(Connection* connection)
    : d(connection->data()), connection_(connection)
{
}

void Connection::deliver(const ax25::Packet& p)
{
    assert(p.has_iframe());
    log() << connection_id_ << " > Delivering data of size "
          << p.iframe().payload().size();
    receive_(p.iframe().payload());
}

void Connection::send_packet(const ax25::Packet& packet)
{
    auto p = packet;
    p.set_command_response_la(!p.command_response());
    p.set_src(src_);
    p.set_dst(dst_);
    send_(p);
}

void Connection::maybe_change_state(std::unique_ptr<ConnectionState>&& st)
{
    if (st) {
        state_ = std::move(st);
        if (state_change_) {
            state_change_(state_.get());
        }
    }

    // Now do a data poll, possibly changing state again.
    st = state_->dl_data_poll();
    if (st) {
        state_ = std::move(st);
        if (state_change_) {
            state_change_(state_.get());
        }
    }
}

} // namespace seqpacket::con

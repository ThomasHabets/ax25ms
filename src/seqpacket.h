#include "scheduler.h"

#include "proto/gen/api.grpc.pb.h"
#include "proto/gen/api.pb.h"

#include <string_view>
#include <deque>

namespace ax25ms {
class Connection
{
public:
    Connection(std::string_view mycall,
               std::string_view peer,
               ax25ms::RouterService::Stub* router,
               Timer* scheduler);

    enum class State {
        IDLE = 0,
        CONNECTING = 1,
        CONNECTED = 2,
        FAILED = 3,
    };


    // No copy.
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Move ok.
    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;

    struct Entry {
        ax25::Packet packet;
        int attempts = 0;
        Timer::time_point_t next_tx;
    };

    grpc::Status write(std::string_view payload);

    std::pair<std::string, grpc::Status> read();

    /*
     * A packet has been added, or a timer has expired.
     * Do something, such as re-send.
     */
    void maybe_send();

    grpc::Status disconnect();

    // Can not call concurrently.
    grpc::Status connect(grpc::ServerContext* ctx);

    void connect_send(grpc::ServerContext* ctx, ax25ms::SendRequest& sreq, int retry = 0);

    bool change_state(State from, State to);
    State get_state();
    void ua(const ax25::Packet& packet);

    void iframe(const ax25::Packet& packet);
    void rr(const ax25::Packet& packet);

    void disc(const ax25::Packet& packet) { std::clog << "disc\n"; }

    void process_acks(const ax25::Packet& packet);

private:
    bool connected_ = false;
    ax25ms::RouterService::Stub* router_;
    Timer* scheduler_;
    std::string mycall_;
    std::string peer_;

    std::mutex send_mu_;
    std::deque<Entry> send_queue_;

    std::mutex mu_;
    std::condition_variable cv_; // Trigger when state changes.
    std::deque<std::string> receive_queue_;
    State state_ = State::IDLE;

    static constexpr int normal_modulus = 8;
    static constexpr int extended_modulus = 128;

    const int modulus_ = normal_modulus;
    int window_size_ = 4; // TODO: what to default to?
    int64_t nr_ = 0;      // expected next packet.
    int64_t nr_sent_ = 0; // Last nc that was sent.
    int64_t ns_ = 0;      // next sequence number to send.

    // call with lock held.
    int nrm() const noexcept { return nr_ % modulus_; }
    int nsm() const noexcept { return ns_ % modulus_; }

    ax25ms::SeqMetadata metadata_;

    grpc::Status send_rr(std::string_view dst, std::string_view src, int n);

    friend class ConnectionTest;
};

} // namespace ax25ms

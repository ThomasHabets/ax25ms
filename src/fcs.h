#include <string_view>
#include <cinttypes>
#include <tuple>

namespace ax25ms {
std::pair<uint8_t, uint8_t> fcs(std::string_view data);

std::tuple<std::string_view, uint16_t, bool> check_fcs(std::string_view data);
} // namespace ax25ms

// The Windows layer composer is intentionally unavailable on Switch. Keep
// the lifecycle symbol used by the common game shutdown path defined.
#include "wr64_composer_window.hpp"

namespace wr64::composer {

void shutdown() {
}

void wait_if_frozen() {
}

} // namespace wr64::composer

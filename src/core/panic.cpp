#include <pfs/common.hpp>

#include <stdexcept>
#include <string>

namespace pfs::detail {

// Rust's `panic!` / `unreachable!`. Thrown rather than aborted so the ported
// tests can observe the failure modes the Rust tests observe, and so a Game
// implementation that forgets a required method reports where.
void unreachable(const char* file, int line, const char* what) {
    throw std::logic_error(std::string(what) + " at " + file + ":" + std::to_string(line));
}

}  // namespace pfs::detail

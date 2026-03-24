////////////////////////////////////////////////////////////////////////////////
// kv8log/kv8log_types.h -- minimal shared type definitions.
//
// CR-19: Extracted here so translation units that only need kv8log_h can
// include this lightweight header without pulling in Channel.h and <mutex>.
////////////////////////////////////////////////////////////////////////////////

#pragma once

namespace kv8log {

// Opaque handle returned by kv8log_open() / Channel::Handle().
using kv8log_h = void*;

} // namespace kv8log

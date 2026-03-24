////////////////////////////////////////////////////////////////////////////////
// kv8log/test/test_compile_out.cpp
//
// Verifies that all kv8log macros expand to nothing / ((void)0) when
// KV8_LOG_ENABLE is NOT defined.  This file intentionally omits the define.
// The test passes if it compiles and links without errors or warnings.
// At runtime it simply confirms that the code paths execute without crashing.
////////////////////////////////////////////////////////////////////////////////

// Deliberately NOT defining KV8_LOG_ENABLE.
#include "kv8log/KV8_Log.h"

#include <cassert>
#include <cstdio>

static void use_macros()
{
    // All of these must compile to nothing.
    KV8_CHANNEL(my_ch, "test/channel");
    KV8_TEL(my_ctr, "test/counter", 0.0, 100.0);
    KV8_TEL_CH(my_ch, my_ctr2, "test/counter2", 0.0, 100.0);
    KV8_TEL_ADD(my_ctr, 42.0);
    KV8_TEL_ADD_CH(my_ch, my_ctr2, 7.0);
    KV8_TEL_ADD_TS(my_ctr, 1.0, 0ULL);
    KV8_TEL_FLUSH();
    (void)KV8_MONO_TO_NS(0ULL);
    KV8_LOG_CONFIGURE("b", "c", "u", "p");
}

int main()
{
    use_macros();
    use_macros();  // call twice: static declarations inside must not redeclare

    fprintf(stdout, "[test_compile_out] PASS -- all macros are no-ops\n");
    return 0;
}

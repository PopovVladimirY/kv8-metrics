////////////////////////////////////////////////////////////////////////////////
// kv8log/test/test_null_lib.cpp
//
// Verifies that 10000 KV8_TEL_ADD() calls complete without crash or exception
// when the runtime shared library is absent.  When the runtime IS present the
// test produces a valid kv8 session visible in kv8scope.
//
// Counter range [0.0, 9999.0] matches the actual data values (0..9999).
////////////////////////////////////////////////////////////////////////////////

#ifndef KV8_LOG_ENABLE
#  define KV8_LOG_ENABLE
#endif
#include "kv8log/KV8_Log.h"

#include <cassert>
#include <cstdio>

static void hammer()
{
    KV8_TEL(null_ctr, "null/counter", 0.0, 9999.0);
    for (int i = 0; i < 10000; ++i)
        KV8_TEL_ADD(null_ctr, (double)i);
    KV8_TEL_FLUSH();
}

int main()
{
    KV8_LOG_CONFIGURE("localhost:19092", "test/null_lib_test",
                      "kv8producer", "kv8secret");

    hammer();
    hammer(); // second call ensures statics are not re-initialized

    fprintf(stdout, "[test_null_lib] PASS -- 20000 Add() calls completed\n");
    return 0;
}

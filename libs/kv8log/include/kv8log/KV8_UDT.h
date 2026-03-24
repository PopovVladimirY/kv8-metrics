////////////////////////////////////////////////////////////////////////////////
// kv8log/KV8_UDT.h -- User Defined Type (UDT) telemetry macros.
//
// Usage (with KV8_LOG_ENABLE defined):
//
//   // 1. Declare a schema using a field-list helper macro (X-macro pattern).
//   //    The helper must accept 4 callbacks: F, FN, E, EN.
//   //      F (type, cname, mn, mx)           -- primitive field
//   //      FN(type, cname, display, mn, mx)  -- primitive, explicit display name
//   //      E (TypeToken, cname)              -- embedded flat UDT field
//   //      EN(prefix, Name, cname)           -- embedded scoped UDT field
//
//   #define KV8_VEC3_FIELDS(F, FN, E, EN)   \
//       F(f64, x, -1000.0, 1000.0)          \
//       F(f64, y, -1000.0, 1000.0)          \
//       F(f64, z, -1000.0, 1000.0)
//   KV8_UDT_DEFINE(Vec3, KV8_VEC3_FIELDS)
//
//   // Scoped schema (schema name = "nav.Vec3"):
//   KV8_UDT_DEFINE_NS(nav, Vec3, KV8_VEC3_FIELDS)
//
//   // 2. Declare a feed (once per TU, e.g. in a function body).
//   KV8_UDT_FEED(my_pos, Vec3, "nav/position");
//
//   // 3. Send a sample.
//   Kv8UDT_Vec3 p; p.x = 1.0; p.y = 2.0; p.z = 3.0;
//   KV8_UDT_ADD(my_pos, p);
//
// Without KV8_LOG_ENABLE all macros expand to nothing / no-op.
// The struct definitions (Kv8UDT_*) are always emitted so user code
// that constructs the struct before a KV8_UDT_ADD call compiles in both
// enabled and disabled builds.
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <chrono>
#include <cstdint>
#include <kv8/Kv8Types.h>

// ---------------------------------------------------------------------------
// Pack pragma helpers -- used internally to generate tightly-packed structs.
// Packing must match the sequential offsets computed by UdtSchemaParser.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#  define KV8_UDT_PACK_PUSH() __pragma(pack(push,1))
#  define KV8_UDT_PACK_POP()  __pragma(pack(pop))
#else
#  define KV8_UDT_PACK_PUSH() _Pragma("pack(push,1)")
#  define KV8_UDT_PACK_POP()  _Pragma("pack(pop)")
#endif

#ifdef KV8_LOG_ENABLE
#  include "kv8log/UdtFeed.h"
#  include <string>
#  include <cstdio>
#endif

////////////////////////////////////////////////////////////////////////////////
// Field type mappings: type-tag token -> C++ type
////////////////////////////////////////////////////////////////////////////////

#define KV8_UDT_CTYPE_i8       int8_t
#define KV8_UDT_CTYPE_u8       uint8_t
#define KV8_UDT_CTYPE_i16      int16_t
#define KV8_UDT_CTYPE_u16      uint16_t
#define KV8_UDT_CTYPE_i32      int32_t
#define KV8_UDT_CTYPE_u32      uint32_t
#define KV8_UDT_CTYPE_i64      int64_t
#define KV8_UDT_CTYPE_u64      uint64_t
#define KV8_UDT_CTYPE_f32      float
#define KV8_UDT_CTYPE_f64      double
#define KV8_UDT_CTYPE_rational kv8::Kv8Rational

////////////////////////////////////////////////////////////////////////////////
// Schema builder (enabled builds only)
////////////////////////////////////////////////////////////////////////////////

#ifdef KV8_LOG_ENABLE

namespace kv8_udt_detail {

// Lightweight JSON accumulator used at static-init time to build schema strings.
// NOT on the hot path; called once per UDT type (function-local static init).
struct SchemaAccumulator
{
    std::string json;
    bool        needs_comma = false;

    SchemaAccumulator& field(const char* type_tag, const char* display_name,
                              double mn, double mx)
    {
        if (needs_comma) json += ',';
        needs_comma = true;
        json += "{\"n\":\"";
        json += display_name;
        json += "\",\"t\":\"";
        json += type_tag;
        json += "\",\"min\":";
        AppendNum(mn);
        json += ",\"max\":";
        AppendNum(mx);
        json += '}';
        return *this;
    }

    // Emit a field with a separate C identifier (path key) and a human-readable
    // display label.  The C identifier is stored as "n" and used as the flattened
    // path segment; the display label is stored as "d" for the UI to pick up.
    SchemaAccumulator& field_named(const char* type_tag, const char* cname_id,
                                   const char* disp_name, double mn, double mx)
    {
        if (needs_comma) json += ',';
        needs_comma = true;
        json += "{\"n\":\"";
        json += cname_id;   // C identifier -- no slashes, safe as path segment
        json += "\",\"d\":\"";
        json += disp_name;  // human-readable label shown in the UI
        json += "\",\"t\":\"";
        json += type_tag;
        json += "\",\"min\":";
        AppendNum(mn);
        json += ",\"max\":";
        AppendNum(mx);
        json += '}';
        return *this;
    }

    SchemaAccumulator& embed(const char* schema_name, const char* display_name)
    {
        if (needs_comma) json += ',';
        needs_comma = true;
        json += "{\"n\":\"";
        json += display_name;
        json += "\",\"t\":\"";
        json += schema_name;
        json += "\"}";
        return *this;
    }

    std::string build(const char* schema_name) const
    {
        std::string s;
        s.reserve(64 + json.size());
        s += "{\"kv8_schema\":1,\"name\":\"";
        s += schema_name;
        s += "\",\"fields\":[";
        s += json;
        s += "]}";
        return s;
    }

private:
    void AppendNum(double v)
    {
        char buf[32];
        long long iv = static_cast<long long>(v);
        int n;
        if (static_cast<double>(iv) == v)
            n = snprintf(buf, sizeof(buf), "%lld", iv);
        else
            n = snprintf(buf, sizeof(buf), "%.17g", v);
        if (n > 0 && n < (int)sizeof(buf))
            json.append(buf, static_cast<size_t>(n));
    }
};

} // namespace kv8_udt_detail

#endif // KV8_LOG_ENABLE

////////////////////////////////////////////////////////////////////////////////
// Per-field expansion callbacks used by KV8_UDT_DEFINE.
// The field-list macro FIELDS(F, FN, E, EN) expands its arguments as:
//   F (type, cname, mn, mx)              primitive field
//   FN(type, cname, display, mn, mx)     primitive with explicit display name
//   E (TypeToken, cname)                 embedded flat UDT field
//   EN(prefix, Name, cname)              embedded scoped UDT field
////////////////////////////////////////////////////////////////////////////////

// -- Struct-member expansions (always) --------------------------------------

#define KV8_UDT__MEMBER(type, cname, mn, mx)           KV8_UDT_CTYPE_##type cname;
#define KV8_UDT__MEMBER_N(type, cname, disp, mn, mx)   KV8_UDT_CTYPE_##type cname;
#define KV8_UDT__EMBED(TypeToken, cname)               Kv8UDT_##TypeToken cname;
#define KV8_UDT__EMBED_N(prefix, Name, cname)          Kv8UDT_##prefix##_##Name cname;

// -- JSON fragment expansions (enabled builds only) --------------------------

#ifdef KV8_LOG_ENABLE

#  define KV8_UDT__JSON_F(type, cname, mn, mx)          \
       _acc.field(#type, #cname,                         \
           static_cast<double>(mn), static_cast<double>(mx));

#  define KV8_UDT__JSON_FN(type, cname, disp, mn, mx)        \
       _acc.field_named(#type, #cname, disp,                  \
           static_cast<double>(mn), static_cast<double>(mx));

#  define KV8_UDT__JSON_E(TypeToken, cname)              \
       _acc.embed(#TypeToken, #cname);

#  define KV8_UDT__JSON_EN(prefix, Name, cname)          \
       _acc.embed(#prefix "." #Name, #cname);

// -- Dependency registration expansion callbacks (enabled builds only) ------
// These are invoked by the generated RegisterDeps_<Name> functions to
// recursively pre-register all embedded sub-schemas before the top-level feed.
// Primitive fields (F, FN) have no schema dependency.

#  define KV8_UDT__REGDEPS_F(type, cname, mn, mx)
#  define KV8_UDT__REGDEPS_FN(type, cname, disp, mn, mx)
#  define KV8_UDT__REGDEPS_E(TypeToken, cname)                         \
       kv8_udt_schema::RegisterDeps_##TypeToken(reg_fn, h);             \
       reg_fn(h, kv8_udt_schema::GetSchema_##TypeToken());
#  define KV8_UDT__REGDEPS_EN(prefix, Name, cname)                     \
       kv8_udt_schema::RegisterDeps_##prefix##_##Name(reg_fn, h);       \
       reg_fn(h, kv8_udt_schema::GetSchema_##prefix##_##Name());

#else

#  define KV8_UDT__JSON_F(type, cname, mn, mx)
#  define KV8_UDT__JSON_FN(type, cname, disp, mn, mx)
#  define KV8_UDT__JSON_E(TypeToken, cname)
#  define KV8_UDT__JSON_EN(prefix, Name, cname)

#endif // KV8_LOG_ENABLE

////////////////////////////////////////////////////////////////////////////////
// KV8_UDT_DEFINE -- primary schema definition macro.
//
// Generates:
//   struct Kv8UDT_<Name> { ... };
//   static_assert(sizeof(...) <= KV8_UDT_MAX_PAYLOAD);
//   (enabled) kv8_udt_schema::GetSchema_<Name>() returning the schema JSON
//
// FIELDS is a macro of the form:
//   #define MY_FIELDS(F, FN, E, EN) F(...) FN(...) E(...) EN(...)
////////////////////////////////////////////////////////////////////////////////

#ifdef KV8_LOG_ENABLE

#define KV8_UDT_DEFINE(Name, FIELDS)                                           \
    KV8_UDT_PACK_PUSH()                                                        \
    struct Kv8UDT_##Name {                                                     \
        FIELDS(KV8_UDT__MEMBER, KV8_UDT__MEMBER_N,                            \
               KV8_UDT__EMBED,  KV8_UDT__EMBED_N)                             \
    };                                                                         \
    KV8_UDT_PACK_POP()                                                         \
    static_assert(sizeof(Kv8UDT_##Name) <= kv8::KV8_UDT_MAX_PAYLOAD,          \
        "Kv8UDT_" #Name " exceeds KV8_UDT_MAX_PAYLOAD (240 bytes)");          \
    namespace kv8_udt_schema {                                                 \
    inline const char* GetSchema_##Name()                                      \
    {                                                                          \
        static const std::string s_json = [&]() -> std::string {              \
            kv8_udt_detail::SchemaAccumulator _acc;                            \
            FIELDS(KV8_UDT__JSON_F,  KV8_UDT__JSON_FN,                        \
                   KV8_UDT__JSON_E,  KV8_UDT__JSON_EN)                        \
            return _acc.build(#Name);                                          \
        }();                                                                   \
        return s_json.c_str();                                                 \
    }                                                                          \
    inline void RegisterDeps_##Name(                                           \
        int (*reg_fn)(void*, const char*), void* h)                            \
    {                                                                          \
        FIELDS(KV8_UDT__REGDEPS_F,  KV8_UDT__REGDEPS_FN,                     \
               KV8_UDT__REGDEPS_E,  KV8_UDT__REGDEPS_EN)                     \
    }                                                                          \
    } /* namespace kv8_udt_schema */

#define KV8_UDT_DEFINE_NS(prefix, Name, FIELDS)                                \
    KV8_UDT_PACK_PUSH()                                                        \
    struct Kv8UDT_##prefix##_##Name {                                          \
        FIELDS(KV8_UDT__MEMBER, KV8_UDT__MEMBER_N,                            \
               KV8_UDT__EMBED,  KV8_UDT__EMBED_N)                             \
    };                                                                         \
    KV8_UDT_PACK_POP()                                                         \
    static_assert(sizeof(Kv8UDT_##prefix##_##Name) <= kv8::KV8_UDT_MAX_PAYLOAD, \
        "Kv8UDT_" #prefix "_" #Name " exceeds KV8_UDT_MAX_PAYLOAD (240 bytes)"); \
    namespace kv8_udt_schema {                                                 \
    inline const char* GetSchema_##prefix##_##Name()                           \
    {                                                                          \
        static const std::string s_json = [&]() -> std::string {              \
            kv8_udt_detail::SchemaAccumulator _acc;                            \
            FIELDS(KV8_UDT__JSON_F,  KV8_UDT__JSON_FN,                        \
                   KV8_UDT__JSON_E,  KV8_UDT__JSON_EN)                        \
            return _acc.build(#prefix "." #Name);                              \
        }();                                                                   \
        return s_json.c_str();                                                 \
    }                                                                          \
    inline void RegisterDeps_##prefix##_##Name(                                \
        int (*reg_fn)(void*, const char*), void* h)                            \
    {                                                                          \
        FIELDS(KV8_UDT__REGDEPS_F,  KV8_UDT__REGDEPS_FN,                     \
               KV8_UDT__REGDEPS_E,  KV8_UDT__REGDEPS_EN)                     \
    }                                                                          \
    } /* namespace kv8_udt_schema */

#else // !KV8_LOG_ENABLE

#define KV8_UDT_DEFINE(Name, FIELDS)                                           \
    KV8_UDT_PACK_PUSH()                                                        \
    struct Kv8UDT_##Name {                                                     \
        FIELDS(KV8_UDT__MEMBER, KV8_UDT__MEMBER_N,                            \
               KV8_UDT__EMBED,  KV8_UDT__EMBED_N)                             \
    };                                                                         \
    KV8_UDT_PACK_POP()                                                         \
    static_assert(sizeof(Kv8UDT_##Name) <= kv8::KV8_UDT_MAX_PAYLOAD,          \
        "Kv8UDT_" #Name " exceeds KV8_UDT_MAX_PAYLOAD (240 bytes)");

#define KV8_UDT_DEFINE_NS(prefix, Name, FIELDS)                                \
    KV8_UDT_PACK_PUSH()                                                        \
    struct Kv8UDT_##prefix##_##Name {                                          \
        FIELDS(KV8_UDT__MEMBER, KV8_UDT__MEMBER_N,                            \
               KV8_UDT__EMBED,  KV8_UDT__EMBED_N)                             \
    };                                                                         \
    KV8_UDT_PACK_POP()                                                         \
    static_assert(sizeof(Kv8UDT_##prefix##_##Name) <= kv8::KV8_UDT_MAX_PAYLOAD, \
        "Kv8UDT_" #prefix "_" #Name " exceeds KV8_UDT_MAX_PAYLOAD (240 bytes)");

#endif // KV8_LOG_ENABLE

////////////////////////////////////////////////////////////////////////////////
// KV8_UDT_BEGIN / KV8_UDT_FIELD / KV8_UDT_END -- struct-only inline syntax.
//
// Generates the C++ struct and static_assert WITHOUT schema JSON.
// For a complete UDT feed (struct + schema + feed), use KV8_UDT_DEFINE.
////////////////////////////////////////////////////////////////////////////////

#define KV8_UDT_BEGIN(Name)              KV8_UDT_PACK_PUSH() struct Kv8UDT_##Name {
#define KV8_UDT_BEGIN_NS(prefix, Name)   KV8_UDT_PACK_PUSH() struct Kv8UDT_##prefix##_##Name {

#define KV8_UDT_FIELD(type, cname, mn, mx)             KV8_UDT_CTYPE_##type cname;
#define KV8_UDT_FIELD_NAMED(type, cname, disp, mn, mx) KV8_UDT_CTYPE_##type cname;
#define KV8_UDT_EMBED(TypeToken, cname)                Kv8UDT_##TypeToken cname;
#define KV8_UDT_EMBED_NAMED(TypeToken, cname, disp)    Kv8UDT_##TypeToken cname;
#define KV8_UDT_EMBED_NS(prefix, Name, cname)          Kv8UDT_##prefix##_##Name cname;

#define KV8_UDT_END(Name)                                                      \
    };                                                                         \
    KV8_UDT_PACK_POP()                                                         \
    static_assert(sizeof(Kv8UDT_##Name) <= kv8::KV8_UDT_MAX_PAYLOAD,          \
        "Kv8UDT_" #Name " exceeds KV8_UDT_MAX_PAYLOAD (240 bytes)");

#define KV8_UDT_END_NS(prefix, Name)                                           \
    };                                                                         \
    KV8_UDT_PACK_POP()                                                         \
    static_assert(sizeof(Kv8UDT_##prefix##_##Name) <= kv8::KV8_UDT_MAX_PAYLOAD, \
        "Kv8UDT_" #prefix "_" #Name " exceeds KV8_UDT_MAX_PAYLOAD (240 bytes)");

////////////////////////////////////////////////////////////////////////////////
// Feed declaration and sample macros
////////////////////////////////////////////////////////////////////////////////

#ifdef KV8_LOG_ENABLE

// Declare a UdtFeed on the default (per-exe) channel.
#define KV8_UDT_FEED(var, TypeToken, display_name)                             \
    static kv8log::UdtFeed<Kv8UDT_##TypeToken> var(                           \
        kv8log::DefaultChannel(), (display_name),                              \
        kv8_udt_schema::GetSchema_##TypeToken,                                 \
        &kv8_udt_schema::RegisterDeps_##TypeToken)

// Declare a UdtFeed on an explicit Channel object.
#define KV8_UDT_FEED_CH(ch, var, TypeToken, display_name)                      \
    static kv8log::UdtFeed<Kv8UDT_##TypeToken> var(                           \
        (ch), (display_name),                                                  \
        kv8_udt_schema::GetSchema_##TypeToken,                                 \
        &kv8_udt_schema::RegisterDeps_##TypeToken)

// Scoped (NS) variants.
#define KV8_UDT_FEED_NS(var, prefix, Name, display_name)                       \
    static kv8log::UdtFeed<Kv8UDT_##prefix##_##Name> var(                     \
        kv8log::DefaultChannel(), (display_name),                              \
        kv8_udt_schema::GetSchema_##prefix##_##Name,                           \
        &kv8_udt_schema::RegisterDeps_##prefix##_##Name)

#define KV8_UDT_FEED_NS_CH(ch, var, prefix, Name, display_name)                \
    static kv8log::UdtFeed<Kv8UDT_##prefix##_##Name> var(                     \
        (ch), (display_name),                                                  \
        kv8_udt_schema::GetSchema_##prefix##_##Name,                           \
        &kv8_udt_schema::RegisterDeps_##prefix##_##Name)

// Record a sample (auto-timestamp, hot path).
#define KV8_UDT_ADD(var, val)                 (var).Add(val)

// Record a sample with a caller-supplied timestamp (nanoseconds, Unix epoch).
#define KV8_UDT_ADD_TS(var, val, ts_ns)       (var).AddTs((val), static_cast<uint64_t>(ts_ns))

// Capture the current wall-clock time as a Unix-epoch nanosecond timestamp.
// Declares:  const uint64_t <ts_ns> = <now>;
// Use once per logical sample group, then pass the same value to every
// KV8_UDT_ADD_TS call in that group to keep feeds time-correlated.
#define KV8_TIME(ts_ns) \
    const uint64_t ts_ns = static_cast<uint64_t>( \
        std::chrono::duration_cast<std::chrono::nanoseconds>( \
            std::chrono::system_clock::now().time_since_epoch()).count())

// Enable / disable the feed.
#define KV8_UDT_ENABLE(var)   (var).Enable()
#define KV8_UDT_DISABLE(var)  (var).Disable()

#else // !KV8_LOG_ENABLE

#define KV8_UDT_FEED(var, TypeToken, display_name)
#define KV8_UDT_FEED_CH(ch, var, TypeToken, display_name)
#define KV8_UDT_FEED_NS(var, prefix, Name, display_name)
#define KV8_UDT_FEED_NS_CH(ch, var, prefix, Name, display_name)
#define KV8_UDT_ADD(var, val)           ((void)0)
#define KV8_UDT_ADD_TS(var, val, ts_ns) ((void)0)
#define KV8_TIME(ts_ns)                 ((void)0) //const uint64_t ts_ns = 0
#define KV8_UDT_ENABLE(var)             ((void)0)
#define KV8_UDT_DISABLE(var)            ((void)0)

#endif // KV8_LOG_ENABLE

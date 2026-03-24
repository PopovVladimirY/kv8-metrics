////////////////////////////////////////////////////////////////////////////////
// UdtSchemaParser.cpp -- Parse UDT schema JSON into ResolvedSchema.
//
// The JSON grammar handled here is a strict subset:
//   json        := object | array | string | number | true | false | null
//   object      := '{' (key ':' value (',' key ':' value)*)? '}'
//   array       := '[' (value (',' value)*)? ']'
//   string      := ('"' [^"]* '"') | ("'" [^']* "'")  (both quote styles accepted)
//   number      := '-'? [0-9]+ ('.' [0-9]+)?
//
// Machine-generated schemas never use Unicode escapes, bool, null, or
// trailing commas, so the parser is intentionally minimal.
////////////////////////////////////////////////////////////////////////////////

#include "UdtSchemaParser.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kv8 {

// ---------------------------------------------------------------------------
// Wire sizes
// ---------------------------------------------------------------------------

uint16_t UdtFieldWireSize(UdtFieldType type)
{
    switch (type)
    {
    case UdtFieldType::I8:  case UdtFieldType::U8:  return 1;
    case UdtFieldType::I16: case UdtFieldType::U16: return 2;
    case UdtFieldType::I32: case UdtFieldType::U32:
    case UdtFieldType::F32:                         return 4;
    case UdtFieldType::I64: case UdtFieldType::U64:
    case UdtFieldType::F64: case UdtFieldType::RATIONAL: return 8;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Field decoding
// ---------------------------------------------------------------------------

double UdtDecodeField(const uint8_t* src, UdtFieldType type)
{
    switch (type)
    {
    case UdtFieldType::I8:  { int8_t   v; memcpy(&v, src, 1); return (double)v; }
    case UdtFieldType::U8:  { uint8_t  v; memcpy(&v, src, 1); return (double)v; }
    case UdtFieldType::I16: { int16_t  v; memcpy(&v, src, 2); return (double)v; }
    case UdtFieldType::U16: { uint16_t v; memcpy(&v, src, 2); return (double)v; }
    case UdtFieldType::I32: { int32_t  v; memcpy(&v, src, 4); return (double)v; }
    case UdtFieldType::U32: { uint32_t v; memcpy(&v, src, 4); return (double)v; }
    case UdtFieldType::I64: { int64_t  v; memcpy(&v, src, 8); return (double)v; }
    case UdtFieldType::U64: { uint64_t v; memcpy(&v, src, 8); return (double)v; }
    case UdtFieldType::F32: { float    v; memcpy(&v, src, 4); return (double)v; }
    case UdtFieldType::F64: { double   v; memcpy(&v, src, 8); return v; }
    case UdtFieldType::RATIONAL:
    {
        int32_t  num; uint32_t den;
        memcpy(&num, src,     4);
        memcpy(&den, src + 4, 4);
        return (den != 0) ? ((double)num / (double)den) : 0.0;
    }
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// Minimal JSON parser
// ---------------------------------------------------------------------------

struct Parser
{
    const char* p;   // current position
    const char* end; // one past the last character
    char errbuf[256];

    bool ok(size_t n = 1) const { return p + n <= end; }

    void skip_ws()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
    }

    // Advance past expected char; return false on mismatch.
    bool consume(char c)
    {
        skip_ws();
        if (p >= end || *p != c)
        {
            snprintf(errbuf, sizeof(errbuf),
                     "expected '%c', got '%c' at offset %d", c, (p < end ? *p : '?'),
                     (int)(p - (end - (end - p))));
            return false;
        }
        ++p;
        return true;
    }

    // Read a JSON string into dst.  Advances past the closing quote.
    // Accepts both double-quote (") and single-quote (') delimiters.
    bool read_string(std::string& dst)
    {
        skip_ws();
        if (p >= end || (*p != '"' && *p != '\''))
        { snprintf(errbuf, sizeof(errbuf), "expected '\"' or ""'""'"); return false; }
        const char q = *p++;  // record opening quote character
        const char* start = p;
        while (p < end && *p != q) ++p;
        if (p >= end) { snprintf(errbuf, sizeof(errbuf), "unterminated string"); return false; }
        dst.assign(start, static_cast<size_t>(p - start));
        ++p; // skip closing quote
        return true;
    }

    // Read a JSON number as double.
    bool read_number(double& val)
    {
        skip_ws();
        if (p >= end) { snprintf(errbuf, sizeof(errbuf), "expected number"); return false; }
        char* endp = nullptr;
        val = strtod(p, &endp);
        if (endp == p) { snprintf(errbuf, sizeof(errbuf), "invalid number"); return false; }
        p = endp;
        return true;
    }

    // Skip any JSON value (used for unknown keys).
    bool skip_value()
    {
        skip_ws();
        if (p >= end) return false;
        if (*p == '"' || *p == '\'')
        {
            std::string dummy;
            return read_string(dummy);
        }
        if (*p == '{' || *p == '[')
        {
            char open = *p, close = (*p == '{') ? '}' : ']';
            int depth = 0;
            ++p;
            while (p < end)
            {
                if (*p == '"' || *p == '\'')
                {
                    std::string dummy;
                    if (!read_string(dummy)) return false;
                    continue;
                }
                if (*p == open)  ++depth;
                if (*p == close) { ++p; if (depth-- == 0) return true; }
                else              ++p;
            }
            snprintf(errbuf, sizeof(errbuf), "unterminated %c", open);
            return false;
        }
        // number, true, false, null
        while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
        return true;
    }
};

// ---------------------------------------------------------------------------
// Type-token string -> UdtFieldType
// ---------------------------------------------------------------------------

static bool ParseTypeToken(const std::string& tok, UdtFieldType& out)
{
    if (tok == "i8")       { out = UdtFieldType::I8;       return true; }
    if (tok == "u8")       { out = UdtFieldType::U8;       return true; }
    if (tok == "i16")      { out = UdtFieldType::I16;      return true; }
    if (tok == "u16")      { out = UdtFieldType::U16;      return true; }
    if (tok == "i32")      { out = UdtFieldType::I32;      return true; }
    if (tok == "u32")      { out = UdtFieldType::U32;      return true; }
    if (tok == "i64")      { out = UdtFieldType::I64;      return true; }
    if (tok == "u64")      { out = UdtFieldType::U64;      return true; }
    if (tok == "f32")      { out = UdtFieldType::F32;      return true; }
    if (tok == "f64")      { out = UdtFieldType::F64;      return true; }
    if (tok == "rat")      { out = UdtFieldType::RATIONAL;  return true; }
    if (tok == "rational") { out = UdtFieldType::RATIONAL;  return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Recursive field flattener
// ---------------------------------------------------------------------------

// Append flattened fields from 'src' into 'out', prefixing each field name
// with 'prefix' and advancing '*offset' by the payload size of each field.
static void FlattenSchema(const ResolvedSchema& src,
                          const std::string&    prefix,
                          uint16_t*             offset,
                          std::vector<UdtSchemaField>& out)
{
    for (const auto& f : src.fields)
    {
        UdtSchemaField sf;
        sf.name    = prefix.empty() ? f.name : (prefix + "." + f.name);
        sf.type    = f.type;
        sf.offset  = *offset;
        sf.min_val = f.min_val;
        sf.max_val = f.max_val;
        out.push_back(sf);
        *offset = static_cast<uint16_t>(*offset + UdtFieldWireSize(f.type));
    }
}

// ---------------------------------------------------------------------------
// ParseUdtSchema
// ---------------------------------------------------------------------------

bool ParseUdtSchema(
    const char*                                   json_buf,
    const std::map<std::string, ResolvedSchema>&  known_schemas,
    ResolvedSchema&                               out,
    std::string&                                  err)
{
    if (!json_buf || *json_buf == '\0')
    {
        err = "empty schema JSON";
        return false;
    }

    Parser ps;
    ps.p   = json_buf;
    ps.end = json_buf + strlen(json_buf);
    ps.errbuf[0] = '\0';

    // Expect top-level object: {"name":"...","fields":[...]}
    if (!ps.consume('{')) { err = ps.errbuf; return false; }

    std::string schema_name;
    std::vector<UdtSchemaField> raw_fields; // before offset assignment

    // Track whether we've seen each key so we can parse in any order.
    bool got_name   = false;
    bool got_fields = false;

    while (true)
    {
        ps.skip_ws();
        if (ps.p >= ps.end) { err = "unexpected end of object"; return false; }
        if (*ps.p == '}') { ++ps.p; break; }

        std::string key;
        if (!ps.read_string(key)) { err = ps.errbuf; return false; }
        if (!ps.consume(':'))     { err = ps.errbuf; return false; }

        if (key == "name")
        {
            if (!ps.read_string(schema_name)) { err = ps.errbuf; return false; }
            got_name = true;
        }
        else if (key == "fields")
        {
            // Array of field objects.
            if (!ps.consume('[')) { err = ps.errbuf; return false; }
            got_fields = true;

            while (true)
            {
                ps.skip_ws();
                if (ps.p >= ps.end) { err = "unexpected end of fields array"; return false; }
                if (*ps.p == ']') { ++ps.p; break; }

                // Each field object: {"n":"...","t":"...","min":...,"max":...}
                // Top-level schema keys use long form ("name", "fields"); per-field
                // keys use the abbreviated form ("n" = field name, "t" = type tag).
                // "min" and "max" are optional when the field is an embedded type.
                if (!ps.consume('{')) { err = ps.errbuf; return false; }

                std::string field_name, type_tok, disp_name;
                double fmin = 0.0, fmax = 0.0;
                bool got_fn = false, got_ft = false;

                while (true)
                {
                    ps.skip_ws();
                    if (ps.p >= ps.end) break;
                    if (*ps.p == '}') { ++ps.p; break; }

                    std::string fkey;
                    if (!ps.read_string(fkey)) { err = ps.errbuf; return false; }
                    if (!ps.consume(':'))       { err = ps.errbuf; return false; }

                    if (fkey == "n")
                    {
                        if (!ps.read_string(field_name)) { err = ps.errbuf; return false; }
                        got_fn = true;
                    }
                    else if (fkey == "d")
                    {
                        if (!ps.read_string(disp_name)) { err = ps.errbuf; return false; }
                    }
                    else if (fkey == "t")
                    {
                        if (!ps.read_string(type_tok)) { err = ps.errbuf; return false; }
                        got_ft = true;
                    }
                    else if (fkey == "min" || fkey == "max")
                    {
                        double v;
                        if (!ps.read_number(v)) { err = ps.errbuf; return false; }
                        if (fkey == "min") fmin = v;
                        else               fmax = v;
                    }
                    else
                    {
                        if (!ps.skip_value()) { err = ps.errbuf; return false; }
                    }

                    ps.skip_ws();
                    if (ps.p < ps.end && *ps.p == ',') ++ps.p;
                }

                if (!got_fn || !got_ft)
                {
                    err = "field object missing 'n' or 't'";
                    return false;
                }

                // Determine if this is a primitive or embedded schema reference.
                UdtFieldType ft;
                if (ParseTypeToken(type_tok, ft))
                {
                    // Primitive field.
                    UdtSchemaField sf;
                    sf.name         = field_name;
                    sf.display_name = disp_name;  // empty when not set (F macro)
                    sf.type    = ft;
                    sf.offset  = 0; // assigned below during flattening
                    sf.min_val = fmin;
                    sf.max_val = fmax;
                    raw_fields.push_back(sf);
                }
                else
                {
                    // Embedded schema reference: type_tok is a schema name.
                    auto sit = known_schemas.find(type_tok);
                    if (sit == known_schemas.end())
                    {
                        err = "unknown embedded schema type: " + type_tok;
                        return false;
                    }
                    // Inline the embedded schema's raw_fields with prefixed names.
                    // We store them as a "virtual" primitive of 0 wire size here
                    // and let FlattenSchema recurse to fill offsets later.
                    // Simpler approach: directly flatten embedded fields now.
                    for (const auto& ef : sit->second.fields)
                    {
                        UdtSchemaField sf;
                        sf.name         = field_name + "." + ef.name;
                        sf.display_name = ef.display_name; // propagate leaf display name
                        sf.type    = ef.type;
                        sf.offset  = 0; // assigned below
                        sf.min_val = ef.min_val;
                        sf.max_val = ef.max_val;
                        raw_fields.push_back(sf);
                    }
                }

                ps.skip_ws();
                if (ps.p < ps.end && *ps.p == ',') ++ps.p;
            }
        }
        else
        {
            // Unknown key -- skip its value.
            if (!ps.skip_value()) { err = ps.errbuf; return false; }
        }

        ps.skip_ws();
        if (ps.p < ps.end && *ps.p == ',') ++ps.p;
    }

    if (!got_name)   { err = "schema JSON missing 'name' field";   return false; }
    if (!got_fields) { err = "schema JSON missing 'fields' field"; return false; }

    // Assign byte offsets sequentially.
    uint16_t offset = 0;
    for (auto& sf : raw_fields)
    {
        sf.offset = offset;
        offset = static_cast<uint16_t>(offset + UdtFieldWireSize(sf.type));
    }

    out.schema_name  = schema_name;
    out.payload_size = offset;
    out.fields       = std::move(raw_fields);
    return true;
}

} // namespace kv8

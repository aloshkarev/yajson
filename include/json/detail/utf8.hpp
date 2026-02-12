#pragma once

/// @file utf8.hpp
/// @author Aleksandr Loshkarev
/// @brief UTF-8 encoding/decoding utilities.
///
/// Full Unicode support:
///   - Encoding a code point to UTF-8 (1-4 bytes)
///   - Decoding UTF-8 to a code point
///   - Encoding a code point as \uXXXX (with surrogate pairs for non-BMP)
///   - UTF-8 sequence validation

#include <cstdint>
#include <string>

namespace yajson::detail::utf8 {

// ─── Code point encoding → UTF-8 ─────────────────────────────────────

/// @brief Encodes a Unicode code point as UTF-8 and appends to the string.
/// @param cp   Unicode code point (0x0000..0x10FFFF).
/// @param out  Destination string for UTF-8 bytes.
inline void encode(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// @brief Encode a code point into a fixed buffer.
/// @return Number of bytes written (1-4), or 0 for an invalid code point.
inline unsigned encode(uint32_t cp, char* buf) noexcept {
    if (cp < 0x80) {
        buf[0] = static_cast<char>(cp);
        return 1;
    } else if (cp < 0x800) {
        buf[0] = static_cast<char>(0xC0 | (cp >> 6));
        buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = static_cast<char>(0xE0 | (cp >> 12));
        buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        buf[0] = static_cast<char>(0xF0 | (cp >> 18));
        buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

// ─── UTF-8 decoding → code point ───────────────────────────────────

/// @brief Determines the UTF-8 sequence length from the leading byte.
/// @return 1-4 for a valid byte, 0 for an invalid one.
inline unsigned sequence_length(unsigned char lead) noexcept {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0; // Invalid leading byte
}

/// @brief Decodes a single UTF-8 sequence.
/// @param ptr  Pointer to the start of the sequence (advanced on return).
/// @param end  Pointer past the end of the buffer.
/// @return Unicode code point, or 0xFFFD on error.
inline uint32_t decode(const char*& ptr, const char* end) noexcept {
    auto lead = static_cast<unsigned char>(*ptr);
    unsigned len = sequence_length(lead);

    if (len == 0 || ptr + len > end) {
        ++ptr;
        return 0xFFFD; // Replacement character
    }

    uint32_t cp;
    switch (len) {
        case 1:
            cp = lead;
            ++ptr;
            return cp;
        case 2:
            cp = lead & 0x1F;
            break;
        case 3:
            cp = lead & 0x0F;
            break;
        case 4:
            cp = lead & 0x07;
            break;
        default:
            ++ptr;
            return 0xFFFD;
    }

    for (unsigned i = 1; i < len; ++i) {
        auto byte = static_cast<unsigned char>(ptr[i]);
        if ((byte & 0xC0) != 0x80) {
            ptr += i;
            return 0xFFFD; // Invalid continuation byte
        }
        cp = (cp << 6) | (byte & 0x3F);
    }

    ptr += len;

    // Check for overlong encoding
    if ((len == 2 && cp < 0x80) ||
        (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000)) {
        return 0xFFFD;
    }

    // Surrogates are invalid in UTF-8
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0xFFFD;

    // Range check
    if (cp > 0x10FFFF) return 0xFFFD;

    return cp;
}

// ─── JSON escaping (\uXXXX) ───────────────────────────────────────────

/// @brief Encodes a code point as \uXXXX (or a surrogate pair for non-BMP).
/// @param cp   Unicode code point.
/// @param out  Destination string for the escape sequence.
inline void encode_escaped(uint32_t cp, std::string& out) {
    static constexpr char kHex[] = "0123456789abcdef";

    auto write_u16 = [&](uint16_t val) {
        out.push_back('\\');
        out.push_back('u');
        out.push_back(kHex[(val >> 12) & 0xF]);
        out.push_back(kHex[(val >> 8) & 0xF]);
        out.push_back(kHex[(val >> 4) & 0xF]);
        out.push_back(kHex[val & 0xF]);
    };

    if (cp <= 0xFFFF) {
        write_u16(static_cast<uint16_t>(cp));
    } else {
        // Surrogate pair for code points > 0xFFFF
        uint32_t adjusted = cp - 0x10000;
        write_u16(static_cast<uint16_t>(0xD800 + (adjusted >> 10)));
        write_u16(static_cast<uint16_t>(0xDC00 + (adjusted & 0x3FF)));
    }
}

// ─── UTF-8 validation ────────────────────────────────────────────────────────

/// @brief Checks whether a string is valid UTF-8.
inline bool validate(const char* ptr, const char* end) noexcept {
    while (ptr < end) {
        auto lead = static_cast<unsigned char>(*ptr);

        if (lead < 0x80) {
            ++ptr;
            continue;
        }

        unsigned len = sequence_length(lead);
        if (len == 0 || ptr + len > end) return false;

        // Check continuation bytes
        for (unsigned i = 1; i < len; ++i) {
            if ((static_cast<unsigned char>(ptr[i]) & 0xC0) != 0x80) {
                return false;
            }
        }

        // Decode and check for overlong encoding + range
        uint32_t cp;
        switch (len) {
            case 2: cp = lead & 0x1F; break;
            case 3: cp = lead & 0x0F; break;
            case 4: cp = lead & 0x07; break;
            default: return false;
        }
        for (unsigned i = 1; i < len; ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(ptr[i]) & 0x3F);
        }

        if ((len == 2 && cp < 0x80) ||
            (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000)) {
            return false; // Overlong encoding
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) return false; // Surrogate
        if (cp > 0x10FFFF) return false; // Beyond Unicode range

        ptr += len;
    }
    return true;
}

} // namespace yajson::detail::utf8

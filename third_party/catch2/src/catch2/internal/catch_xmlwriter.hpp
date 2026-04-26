
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_XMLWRITER_HPP_INCLUDED
#define CATCH_XMLWRITER_HPP_INCLUDED

#include <catch2/internal/catch_lifetimebound.hpp>
#include <catch2/internal/catch_reusable_string_stream.hpp>
#include <catch2/internal/catch_stringref.hpp>

#include <iosfwd>
#include <vector>
#include <cstdint>

namespace Catch {
enum class XmlFormatting : std::uint8_t {
    None = 0x00,
    Indent = 0x01,
    Newline = 0x02,
};

constexpr XmlFormatting operator|(XmlFormatting lhs, XmlFormatting rhs) {
    return static_cast<XmlFormatting>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

constexpr XmlFormatting operator&(XmlFormatting lhs, XmlFormatting rhs) {
    return static_cast<XmlFormatting>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

class XmlEncode {
   public:
    enum ForWhat {
        ForTextNodes,
        ForAttributes
    };

    constexpr XmlEncode(StringRef str CATCH_ATTR_LIFETIMEBOUND, ForWhat forWhat = ForTextNodes)
        : m_str(str), m_forWhat(forWhat) {}

    void encodeTo(std::ostream& os) const;

    friend std::ostream& operator<<(std::ostream& os, XmlEncode const& xmlEncode);

   private:
    StringRef m_str;
    ForWhat m_forWhat;
};

class XmlWriter {
   public:
    class ScopedElement {
       public:
        ScopedElement(XmlWriter* writer CATCH_ATTR_LIFETIMEBOUND, XmlFormatting fmt);

        ScopedElement(ScopedElement&& other) noexcept;
        ScopedElement& operator=(ScopedElement&& other) noexcept;

        ~ScopedElement();

        ScopedElement& writeText(StringRef text, XmlFormatting fmt = XmlFormatting::Newline | XmlFormatting::Indent);

        ScopedElement& writeAttribute(StringRef name, StringRef attribute);
        template <typename T, typename = typename std::enable_if_t<!std::is_convertible<T, StringRef>::value>>
        ScopedElement& writeAttribute(StringRef name, T const& attribute) {
            m_writer->writeAttribute(name, attribute);
            return *this;
        }

       private:
        XmlWriter* m_writer = nullptr;
        XmlFormatting m_fmt;
    };

    XmlWriter(std::ostream& os CATCH_ATTR_LIFETIMEBOUND);
    ~XmlWriter();

    XmlWriter(XmlWriter const&) = delete;
    XmlWriter& operator=(XmlWriter const&) = delete;

    XmlWriter& startElement(std::string const& name,
                            XmlFormatting fmt = XmlFormatting::Newline | XmlFormatting::Indent);

    ScopedElement scopedElement(std::string const& name,
                                XmlFormatting fmt = XmlFormatting::Newline | XmlFormatting::Indent);

    XmlWriter& endElement(XmlFormatting fmt = XmlFormatting::Newline | XmlFormatting::Indent);

    XmlWriter& writeAttribute(StringRef name, StringRef attribute);

    XmlWriter& writeAttribute(StringRef name, bool attribute);

    XmlWriter& writeAttribute(StringRef name, char const* attribute);

    template <typename T, typename = typename std::enable_if_t<!std::is_convertible<T, StringRef>::value>>
    XmlWriter& writeAttribute(StringRef name, T const& attribute) {
        ReusableStringStream rss;
        rss << attribute;
        return writeAttribute(name, rss.str());
    }

    XmlWriter& writeText(StringRef text, XmlFormatting fmt = XmlFormatting::Newline | XmlFormatting::Indent);

    XmlWriter& writeComment(StringRef text, XmlFormatting fmt = XmlFormatting::Newline | XmlFormatting::Indent);

    void writeStylesheetRef(StringRef url);

    void ensureTagClosed();

   private:
    void applyFormatting(XmlFormatting fmt);

    void writeDeclaration();

    void newlineIfNecessary();

    bool m_tagIsOpen = false;
    bool m_needsNewline = false;
    std::vector<std::string> m_tags;
    std::string m_indent;
    std::ostream& m_os;
};

}  // namespace Catch

#endif  // CATCH_XMLWRITER_HPP_INCLUDED

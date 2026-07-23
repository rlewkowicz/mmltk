

#ifndef CATCH_TEXTFLOW_HPP_INCLUDED
#define CATCH_TEXTFLOW_HPP_INCLUDED

#include <catch2/internal/catch_console_width.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

#include <cassert>
#include <string>
#include <vector>

namespace Catch {
namespace TextFlow {

class Columns;

class AnsiSkippingString {
    std::string m_string;
    std::size_t m_size = 0;

    void preprocessString();

   public:
    class const_iterator;
    using iterator = const_iterator;
    static constexpr char sentinel = static_cast<char>(0xffu);

    explicit AnsiSkippingString(std::string const& text);
    explicit AnsiSkippingString(std::string&& text);

    const_iterator begin() const;
    const_iterator end() const;

    size_t size() const {
        return m_size;
    }

    std::string substring(const_iterator begin, const_iterator end) const;
};

class AnsiSkippingString::const_iterator {
    friend AnsiSkippingString;
    struct EndTag {};

    const std::string* m_string;
    std::string::const_iterator m_it;

    explicit const_iterator(const std::string& string, EndTag) : m_string(&string), m_it(string.end()) {}

    void tryParseAnsiEscapes();
    void advance();
    void unadvance();

   public:
    using difference_type = std::ptrdiff_t;
    using value_type = char;
    using pointer = value_type*;
    using reference = value_type&;
    using iterator_category = std::bidirectional_iterator_tag;

    explicit const_iterator(const std::string& string) : m_string(&string), m_it(string.begin()) {
        tryParseAnsiEscapes();
    }

    char operator*() const {
        return *m_it;
    }

    const_iterator& operator++() {
        advance();
        return *this;
    }
    const_iterator operator++(int) {
        iterator prev(*this);
        operator++();
        return prev;
    }
    const_iterator& operator--() {
        unadvance();
        return *this;
    }
    const_iterator operator--(int) {
        iterator prev(*this);
        operator--();
        return prev;
    }

    bool operator==(const_iterator const& other) const {
        return m_it == other.m_it;
    }
    bool operator!=(const_iterator const& other) const {
        return !operator==(other);
    }
    bool operator<=(const_iterator const& other) const {
        return m_it <= other.m_it;
    }

    const_iterator oneBefore() const {
        auto it = *this;
        return --it;
    }
};

class Column {
    AnsiSkippingString m_string;
    size_t m_width = CATCH_CONFIG_CONSOLE_WIDTH - 1;
    size_t m_indent = 0;
    size_t m_initialIndent = std::string::npos;

   public:
    class const_iterator {
        friend Column;
        struct EndTag {};

        Column const& m_column;
        AnsiSkippingString::const_iterator m_lineStart;
        AnsiSkippingString::const_iterator m_lineEnd;
        AnsiSkippingString::const_iterator m_parsedTo;
        bool m_addHyphen = false;

        const_iterator(Column const& column, EndTag)
            : m_column(column),
              m_lineStart(m_column.m_string.end()),
              m_lineEnd(column.m_string.end()),
              m_parsedTo(column.m_string.end()) {}

        void calcLength();

        size_t indentSize() const;

        std::string addIndentAndSuffix(AnsiSkippingString::const_iterator start,
                                       AnsiSkippingString::const_iterator end) const;

       public:
        using difference_type = std::ptrdiff_t;
        using value_type = std::string;
        using pointer = value_type*;
        using reference = value_type&;
        using iterator_category = std::forward_iterator_tag;

        explicit const_iterator(Column const& column);

        std::string operator*() const;

        const_iterator& operator++();
        const_iterator operator++(int);

        bool operator==(const_iterator const& other) const {
            return m_lineStart == other.m_lineStart && &m_column == &other.m_column;
        }
        bool operator!=(const_iterator const& other) const {
            return !operator==(other);
        }
    };
    using iterator = const_iterator;

    explicit Column(std::string const& text) : m_string(text) {}
    explicit Column(std::string&& text) : m_string(CATCH_MOVE(text)) {}

    Column& width(size_t newWidth) & {
        assert(newWidth > 0);
        m_width = newWidth;
        return *this;
    }
    Column&& width(size_t newWidth) && {
        assert(newWidth > 0);
        m_width = newWidth;
        return CATCH_MOVE(*this);
    }
    Column& indent(size_t newIndent) & {
        m_indent = newIndent;
        return *this;
    }
    Column&& indent(size_t newIndent) && {
        m_indent = newIndent;
        return CATCH_MOVE(*this);
    }
    Column& initialIndent(size_t newIndent) & {
        m_initialIndent = newIndent;
        return *this;
    }
    Column&& initialIndent(size_t newIndent) && {
        m_initialIndent = newIndent;
        return CATCH_MOVE(*this);
    }

    size_t width() const {
        return m_width;
    }
    const_iterator begin() const {
        return const_iterator(*this);
    }
    const_iterator end() const {
        return {*this, const_iterator::EndTag{}};
    }

    friend std::ostream& operator<<(std::ostream& os, Column const& col);

    friend Columns operator+(Column const& lhs, Column const& rhs);
    friend Columns operator+(Column&& lhs, Column&& rhs);
};

Column Spacer(size_t spaceWidth);

class Columns {
    std::vector<Column> m_columns;

   public:
    class iterator {
        friend Columns;
        struct EndTag {};

        std::vector<Column> const& m_columns;
        std::vector<Column::const_iterator> m_iterators;
        size_t m_activeIterators;

        iterator(Columns const& columns, EndTag);

       public:
        using difference_type = std::ptrdiff_t;
        using value_type = std::string;
        using pointer = value_type*;
        using reference = value_type&;
        using iterator_category = std::forward_iterator_tag;

        explicit iterator(Columns const& columns);

        auto operator==(iterator const& other) const -> bool {
            return m_iterators == other.m_iterators;
        }
        auto operator!=(iterator const& other) const -> bool {
            return m_iterators != other.m_iterators;
        }
        std::string operator*() const;
        iterator& operator++();
        iterator operator++(int);
    };
    using const_iterator = iterator;

    iterator begin() const {
        return iterator(*this);
    }
    iterator end() const {
        return {*this, iterator::EndTag()};
    }

    friend Columns& operator+=(Columns& lhs, Column const& rhs);
    friend Columns& operator+=(Columns& lhs, Column&& rhs);
    friend Columns operator+(Columns const& lhs, Column const& rhs);
    friend Columns operator+(Columns&& lhs, Column&& rhs);

    friend std::ostream& operator<<(std::ostream& os, Columns const& cols);
};

}  
}  
#endif  // CATCH_TEXTFLOW_HPP_INCLUDED

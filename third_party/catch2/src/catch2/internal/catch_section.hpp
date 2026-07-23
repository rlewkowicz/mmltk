

#ifndef CATCH_SECTION_HPP_INCLUDED
#define CATCH_SECTION_HPP_INCLUDED

#include <catch2/internal/catch_compiler_capabilities.hpp>
#include <catch2/internal/catch_config_static_analysis_support.hpp>
#include <catch2/internal/catch_noncopyable.hpp>
#include <catch2/catch_section_info.hpp>
#include <catch2/catch_timer.hpp>
#include <catch2/catch_totals.hpp>
#include <catch2/internal/catch_unique_name.hpp>

namespace Catch {

class Section : Detail::NonCopyable {
   public:
    Section(SectionInfo&& info);
    Section(SourceLineInfo const& _lineInfo, StringRef _name, const char* const = nullptr);
    ~Section();

    explicit operator bool() const;

   private:
    SectionInfo m_info;

    Counts m_assertions;
    bool m_sectionIncluded;
    Timer m_timer;
};

}  

#if !defined(CATCH_CONFIG_EXPERIMENTAL_STATIC_ANALYSIS_SUPPORT)
#define INTERNAL_CATCH_SECTION(...)                                                \
    CATCH_INTERNAL_START_WARNINGS_SUPPRESSION                                      \
    CATCH_INTERNAL_SUPPRESS_UNUSED_VARIABLE_WARNINGS                               \
    if (Catch::Section const& INTERNAL_CATCH_UNIQUE_NAME(catch_internal_Section) = \
            Catch::Section(CATCH_INTERNAL_LINEINFO, __VA_ARGS__))                  \
    CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION

#define INTERNAL_CATCH_DYNAMIC_SECTION(...)                                                                    \
    CATCH_INTERNAL_START_WARNINGS_SUPPRESSION                                                                  \
    CATCH_INTERNAL_SUPPRESS_UNUSED_VARIABLE_WARNINGS                                                           \
    if (Catch::Section const& INTERNAL_CATCH_UNIQUE_NAME(catch_internal_Section) =                             \
            Catch::SectionInfo(CATCH_INTERNAL_LINEINFO, (Catch::ReusableStringStream() << __VA_ARGS__).str())) \
    CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION

#else

namespace Catch {
namespace Detail {
int GetNewSectionHint(StringRef, const char* const = nullptr);
}
}  

#define INTERNAL_CATCH_SECTION(...)                                                             \
    CATCH_INTERNAL_START_WARNINGS_SUPPRESSION                                                   \
    CATCH_INTERNAL_SUPPRESS_UNUSED_VARIABLE_WARNINGS                                            \
    CATCH_INTERNAL_SUPPRESS_SHADOW_WARNINGS                                                     \
    if ([[maybe_unused]] const int catchInternalPreviousSectionHint = catchInternalSectionHint, \
        catchInternalSectionHint = Catch::Detail::GetNewSectionHint(__VA_ARGS__);               \
        catchInternalPreviousSectionHint == __LINE__)                                           \
    CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION

#define INTERNAL_CATCH_DYNAMIC_SECTION(...)                                                         \
    CATCH_INTERNAL_START_WARNINGS_SUPPRESSION                                                       \
    CATCH_INTERNAL_SUPPRESS_UNUSED_VARIABLE_WARNINGS                                                \
    CATCH_INTERNAL_SUPPRESS_SHADOW_WARNINGS                                                         \
    if ([[maybe_unused]] const int catchInternalPreviousSectionHint = catchInternalSectionHint,     \
        catchInternalSectionHint =                                                                  \
            Catch::Detail::GetNewSectionHint((Catch::ReusableStringStream() << __VA_ARGS__).str()); \
        catchInternalPreviousSectionHint == __LINE__)                                               \
    CATCH_INTERNAL_STOP_WARNINGS_SUPPRESSION

#endif

#endif  // CATCH_SECTION_HPP_INCLUDED

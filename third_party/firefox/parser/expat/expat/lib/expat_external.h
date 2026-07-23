/*
                            __  __            _
                         ___\ \/ /_ __   __ _| |_
                        / _ \\  /| '_ \ / _` | __|
                       |  __//  \| |_) | (_| | |_
                        \___/_/\_\ .__/ \__,_|\__|
                                 |_| XML parser

   Copyright (c) 1997-2000 Thai Open Source Software Center Ltd
   Copyright (c) 2000      Clark Cooper <coopercc@users.sourceforge.net>
   Copyright (c) 2000-2004 Fred L. Drake, Jr. <fdrake@users.sourceforge.net>
   Copyright (c) 2001-2002 Greg Stein <gstein@users.sourceforge.net>
   Copyright (c) 2002-2006 Karl Waclawek <karl@waclawek.net>
   Copyright (c) 2016      Cristian Rodríguez <crrodriguez@opensuse.org>
   Copyright (c) 2016-2025 Sebastian Pipping <sebastian@pipping.org>
   Copyright (c) 2017      Rhodri James <rhodri@wildebeest.org.uk>
   Copyright (c) 2018      Yury Gribov <tetra2005@gmail.com>
   Copyright (c) 2026      Matthew Fernandez <matthew.fernandez@gmail.com>
   Licensed under the MIT license:

   Permission is  hereby granted,  free of charge,  to any  person obtaining
   a  copy  of  this  software   and  associated  documentation  files  (the
   "Software"),  to  deal in  the  Software  without restriction,  including
   without  limitation the  rights  to use,  copy,  modify, merge,  publish,
   distribute, sublicense, and/or sell copies of the Software, and to permit
   persons  to whom  the Software  is  furnished to  do so,  subject to  the
   following conditions:

   The above copyright  notice and this permission notice  shall be included
   in all copies or substantial portions of the Software.

   THE  SOFTWARE  IS  PROVIDED  "AS  IS",  WITHOUT  WARRANTY  OF  ANY  KIND,
   EXPRESS  OR IMPLIED,  INCLUDING  BUT  NOT LIMITED  TO  THE WARRANTIES  OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
   NO EVENT SHALL THE AUTHORS OR  COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
   DAMAGES OR  OTHER LIABILITY, WHETHER  IN AN  ACTION OF CONTRACT,  TORT OR
   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
   USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#if !defined(Expat_External_INCLUDED)
#  define Expat_External_INCLUDED 1


#if !defined(XMLCALL)
#if defined(_MSC_VER)
#      define XMLCALL __cdecl
#elif defined(__GNUC__) && defined(__i386) && ! defined(__INTEL_COMPILER)
#      define XMLCALL __attribute__((cdecl))
#else
#      define XMLCALL
#endif
#endif

#if ! defined(XML_STATIC) && ! defined(XMLIMPORT)
#if !defined(XML_BUILDING_EXPAT)

#if defined(_MSC_VER) && ! defined(__BEOS__) && ! 0
#        define XMLIMPORT __declspec(dllimport)
#endif

#endif
#endif

#if !defined(XML_ENABLE_VISIBILITY)
#    define XML_ENABLE_VISIBILITY 0
#endif

#if ! defined(XMLIMPORT) && XML_ENABLE_VISIBILITY
#    define XMLIMPORT __attribute__((visibility("default")))
#endif

#if !defined(XMLIMPORT)
#    define XMLIMPORT
#endif

#if defined(__GNUC__)                                                        \
      && (__GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 96))
#    define XML_ATTR_MALLOC __attribute__((__malloc__))
#else
#    define XML_ATTR_MALLOC
#endif

#if defined(__GNUC__)                                                        \
      && ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 3))
#    define XML_ATTR_ALLOC_SIZE(x) __attribute__((__alloc_size__(x)))
#else
#    define XML_ATTR_ALLOC_SIZE(x)
#endif

#  define XMLPARSEAPI(type) XMLIMPORT type XMLCALL

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(XML_UNICODE_WCHAR_T)
#if !defined(XML_UNICODE)
#      define XML_UNICODE
#endif
#if defined(__SIZEOF_WCHAR_T__) && (__SIZEOF_WCHAR_T__ != 2)
#      error "sizeof(wchar_t) != 2; Need -fshort-wchar for both Expat and libc"
#endif
#endif


#if defined(XML_LARGE_SIZE)
typedef long long XML_Index;
typedef unsigned long long XML_Size;
#else
typedef long XML_Index;
typedef unsigned long XML_Size;
#endif

#if defined(__cplusplus)
}
#endif

#endif

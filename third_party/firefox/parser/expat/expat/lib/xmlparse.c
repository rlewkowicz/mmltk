/* 5de44e6750c6cc78818f06ed552f522a1241df0299395250e1792cb339389daf (2.8.2+)
                            __  __            _
                         ___\ \/ /_ __   __ _| |_
                        / _ \\  /| '_ \ / _` | __|
                       |  __//  \| |_) | (_| | |_
                        \___/_/\_\ .__/ \__,_|\__|
                                 |_| XML parser

   Copyright (c) 1997-2000 Thai Open Source Software Center Ltd
   Copyright (c) 2000      Clark Cooper <coopercc@users.sourceforge.net>
   Copyright (c) 2000-2006 Fred L. Drake, Jr. <fdrake@users.sourceforge.net>
   Copyright (c) 2001-2002 Greg Stein <gstein@users.sourceforge.net>
   Copyright (c) 2002-2016 Karl Waclawek <karl@waclawek.net>
   Copyright (c) 2005-2009 Steven Solie <steven@solie.ca>
   Copyright (c) 2016      Eric Rahm <erahm@mozilla.com>
   Copyright (c) 2016-2026 Sebastian Pipping <sebastian@pipping.org>
   Copyright (c) 2016      Gaurav <g.gupta@samsung.com>
   Copyright (c) 2016      Thomas Beutlich <tc@tbeu.de>
   Copyright (c) 2016      Gustavo Grieco <gustavo.grieco@imag.fr>
   Copyright (c) 2016      Pascal Cuoq <cuoq@trust-in-soft.com>
   Copyright (c) 2016      Ed Schouten <ed@nuxi.nl>
   Copyright (c) 2017-2022 Rhodri James <rhodri@wildebeest.org.uk>
   Copyright (c) 2017      Václav Slavík <vaclav@slavik.io>
   Copyright (c) 2017      Viktor Szakats <commit@vsz.me>
   Copyright (c) 2017      Chanho Park <chanho61.park@samsung.com>
   Copyright (c) 2017      Rolf Eike Beer <eike@sf-mail.de>
   Copyright (c) 2017      Hans Wennborg <hans@chromium.org>
   Copyright (c) 2018      Anton Maklakov <antmak.pub@gmail.com>
   Copyright (c) 2018      Benjamin Peterson <benjamin@python.org>
   Copyright (c) 2018      Marco Maggi <marco.maggi-ipsu@poste.it>
   Copyright (c) 2018      Mariusz Zaborski <oshogbo@vexillium.org>
   Copyright (c) 2019      David Loffredo <loffredo@steptools.com>
   Copyright (c) 2019-2020 Ben Wagner <bungeman@chromium.org>
   Copyright (c) 2019      Vadim Zeitlin <vadim@zeitlins.org>
   Copyright (c) 2021      Donghee Na <donghee.na@python.org>
   Copyright (c) 2022      Samanta Navarro <ferivoz@riseup.net>
   Copyright (c) 2022      Jeffrey Walton <noloader@gmail.com>
   Copyright (c) 2022      Jann Horn <jannh@google.com>
   Copyright (c) 2022      Sean McBride <sean@rogue-research.com>
   Copyright (c) 2023      Owain Davies <owaind@bath.edu>
   Copyright (c) 2023-2024 Sony Corporation / Snild Dolkow <snild@sony.com>
   Copyright (c) 2024-2025 Berkay Eren Ürün <berkay.ueruen@siemens.com>
   Copyright (c) 2024      Hanno Böck <hanno@gentoo.org>
   Copyright (c) 2025-2026 Matthew Fernandez <matthew.fernandez@gmail.com>
   Copyright (c) 2025      Atrem Borovik <polzovatellllk@gmail.com>
   Copyright (c) 2025      Alfonso Gregory <gfunni234@gmail.com>
   Copyright (c) 2026      Rosen Penev <rosenp@gmail.com>
   Copyright (c) 2026      Francesco Bertolaccini
   Copyright (c) 2026      Christian Ng <christianrng@berkeley.edu>
   Copyright (c) 2026      Nick Begg <nick@stunttruck.net>
   Copyright (c) 2026      Kartik Kenchi <netliomax25@gmail.com>
   Copyright (c) 2026      Haris Hussain <hextheshadow0x@gmail.com>
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

#define XML_BUILDING_EXPAT 1

#include "expat_config.h"

#if ! defined(XML_GE) || (1 - XML_GE - 1 == 2) || (XML_GE < 0) || (XML_GE > 1)
#  error XML_GE (for general entities) must be defined, non-empty, either 1 or 0 (0 to disable, 1 to enable; 1 is a common default)
#endif

#if defined(XML_DTD) && XML_GE == 0
#  error Either undefine XML_DTD or define XML_GE to 1.
#endif

#if ! defined(XML_CONTEXT_BYTES) || (1 - XML_CONTEXT_BYTES - 1 == 2)           \
    || (XML_CONTEXT_BYTES + 0 < 0)
#  error XML_CONTEXT_BYTES must be defined, non-empty and >=0 (0 to disable, >=1 to enable; 1024 is a common default)
#endif

#include <stdbool.h>
#include <stddef.h>
#include <string.h> /* memset(), memcpy() */
#include <assert.h>
#include <limits.h> /* INT_MAX, LLONG_MAX, LONG_MAX, UINT_MAX */
#include <stdio.h>  /* fprintf */
#include <stdlib.h> /* getenv */
#include <stdint.h> /* SIZE_MAX, uintptr_t */
#include <math.h>   /* isnan */
#include <errno.h>

#  include <sys/time.h>  /* gettimeofday() */
#  include <sys/types.h> /* getpid() */
#  include <unistd.h>    /* getpid() */
#  include <fcntl.h>     /* O_RDONLY */
#  include <errno.h>


#include "ascii.h"
#include "expat.h"
#include "siphash.h"
#include "xcsinc.c"


#if ! defined(HAVE_GETRANDOM) && ! defined(HAVE_SYSCALL_GETRANDOM)             \
    && ! defined(HAVE_ARC4RANDOM_BUF) && ! defined(HAVE_ARC4RANDOM)            \
    && ! defined(HAVE_GETENTROPY) && ! defined(XML_DEV_URANDOM)                \
    && ! 0 && ! defined(XML_POOR_ENTROPY)
#  error You do not have support for any sources of high quality entropy \
    enabled.  For end user security, that is probably not what you want. \
    \
    Your options include: \
      * Linux >=3.17 + glibc >=2.25 (getrandom): HAVE_GETRANDOM, \
      * Linux >=3.17 + glibc (including <2.25) (syscall SYS_getrandom): HAVE_SYSCALL_GETRANDOM, \
      * BSD / macOS >=10.7 / glibc >=2.36 (arc4random_buf): HAVE_ARC4RANDOM_BUF, \
      * BSD / macOS (including <10.7) / glibc >=2.36 (arc4random): HAVE_ARC4RANDOM, \
      * BSD / macOS >=10.12 / glibc >=2.25 (getentropy): HAVE_GETENTROPY, \
      * Linux (including <3.17) / BSD / macOS (including <10.7) / Solaris >=8 (/dev/urandom): XML_DEV_URANDOM, \
      * Windows >=Vista (rand_s): _WIN32. \
    \
    If you insist on not using any of these, bypass this error by defining \
    XML_POOR_ENTROPY and be vulnerable to hash flooding; you have been warned. \
    \
    If you have reasons to patch this detection code away or need changes \
    to the build system, please open a bug.  Thank you!
#endif

#if defined(XML_UNICODE)
#  define XML_ENCODE_MAX XML_UTF16_ENCODE_MAX
#  define XmlConvert XmlUtf16Convert
#  define XmlGetInternalEncoding XmlGetUtf16InternalEncoding
#  define XmlGetInternalEncodingNS XmlGetUtf16InternalEncodingNS
#  define XmlEncode XmlUtf16Encode
#  define MUST_CONVERT(enc, s) (! (enc)->isUtf16 || (((uintptr_t)(s)) & 1))
typedef unsigned short ICHAR;
#else
#  define XML_ENCODE_MAX XML_UTF8_ENCODE_MAX
#  define XmlConvert XmlUtf8Convert
#  define XmlGetInternalEncoding XmlGetUtf8InternalEncoding
#  define XmlGetInternalEncodingNS XmlGetUtf8InternalEncodingNS
#  define XmlEncode XmlUtf8Encode
#  define MUST_CONVERT(enc, s) (! (enc)->isUtf8)
typedef char ICHAR;
#endif

#if !defined(XML_NS)

#  define XmlInitEncodingNS XmlInitEncoding
#  define XmlInitUnknownEncodingNS XmlInitUnknownEncoding
#  undef XmlGetInternalEncodingNS
#  define XmlGetInternalEncodingNS XmlGetInternalEncoding
#  define XmlParseXmlDeclNS XmlParseXmlDecl

#endif


#if defined(XML_LARGE_SIZE)
#  define XML_INDEX_MAX LLONG_MAX
#else
#  define XML_INDEX_MAX LONG_MAX
#endif

#define ROUND_UP(n, sz) (((n) + ((sz) - 1)) & ~((sz) - 1))

#define EXPAT_SAFE_PTR_DIFF(p, q) (((p) && (q)) ? ((p) - (q)) : 0)

#define EXPAT_MIN(a, b) (((a) < (b)) ? (a) : (b))

#include "internal.h"
#include "xmltok.h"
#include "xmlrole.h"

typedef const XML_Char *KEY;

typedef struct {
  KEY name;
} NAMED;

typedef struct {
  NAMED **v;
  unsigned char power;
  size_t size;
  size_t used;
  XML_Parser parser;
} HASH_TABLE;

static size_t keylen(KEY s);

static void copy_salt_to_sipkey(XML_Parser parser, struct sipkey *key);

#define SECOND_HASH(hash, mask, power)                                         \
  ((((hash) & ~(mask)) >> ((power) - 1)) & ((mask) >> 2))
#define PROBE_STEP(hash, mask, power)                                          \
  ((unsigned char)((SECOND_HASH(hash, mask, power)) | 1))

typedef struct {
  NAMED **p;
  NAMED **end;
} HASH_TABLE_ITER;

#define INIT_TAG_BUF_SIZE 32 /* must be a multiple of sizeof(XML_Char) */
#define INIT_DATA_BUF_SIZE 1024
#define INIT_ATTS_SIZE 16
#define INIT_ATTS_VERSION 0xFFFFFFFF
#define INIT_BLOCK_SIZE ((int)(1024 - (offsetof(BLOCK, s) / sizeof(XML_Char))))
#define INIT_BUFFER_SIZE 1024

#define EXPAND_SPARE 24

typedef struct binding {
  struct prefix *prefix;
  struct binding *nextTagBinding;
  struct binding *prevPrefixBinding;
  const struct attribute_id *attId;
  XML_Char *uri;
  size_t uriLen;
  size_t uriAlloc;
} BINDING;

typedef struct prefix {
  const XML_Char *name;
  BINDING *binding;
} PREFIX;

typedef struct {
  const XML_Char *str;
  const XML_Char *localPart;
  const XML_Char *prefix;
  size_t strLen;
  size_t uriLen;
  size_t prefixLen;
} TAG_NAME;

typedef struct tag {
  struct tag *parent;  
  const char *rawName; 
  int rawNameLength;
  TAG_NAME name; 
  union {
    char *raw;     
    XML_Char *str; 
  } buf;           
  char *bufEnd;    
  BINDING *bindings;
} TAG;

typedef struct {
  const XML_Char *name;
  const XML_Char *textPtr;
  int textLen;   
  int processed; 
  const XML_Char *systemId;
  const XML_Char *base;
  const XML_Char *publicId;
  const XML_Char *notation;
  XML_Bool open;
  XML_Bool hasMore; 
  XML_Bool is_param;
  XML_Bool is_internal; 
} ENTITY;

typedef struct {
  enum XML_Content_Type type;
  enum XML_Content_Quant quant;
  const XML_Char *name;
  int firstchild;
  int lastchild;
  int childcnt;
  int nextsib;
} CONTENT_SCAFFOLD;

#define INIT_SCAFFOLD_ELEMENTS 32

typedef struct block {
  struct block *next;
  int size;
  XML_Char s[];
} BLOCK;

typedef struct {
  BLOCK *blocks;
  BLOCK *freeBlocks;
  const XML_Char *end;
  XML_Char *ptr;
  XML_Char *start;
  XML_Parser parser;
} STRING_POOL;

typedef struct attribute_id {
  XML_Char *name;
  PREFIX *prefix;
  XML_Bool maybeTokenized;
  XML_Bool xmlns;
} ATTRIBUTE_ID;

typedef struct {
  const ATTRIBUTE_ID *id;
  XML_Bool isCdata;
  const XML_Char *value;
} DEFAULT_ATTRIBUTE;

typedef struct {
  unsigned long version;
  unsigned long hash;
  const XML_Char *uriName;
} NS_ATT;

typedef struct {
  const XML_Char *name;
  PREFIX *prefix;
  const ATTRIBUTE_ID *idAtt;
  size_t nDefaultAtts;
  size_t allocDefaultAtts;
  DEFAULT_ATTRIBUTE *defaultAtts;
  HASH_TABLE defaultAttsNames;
} ELEMENT_TYPE;

typedef struct {
  HASH_TABLE generalEntities;
  HASH_TABLE elementTypes;
  HASH_TABLE attributeIds;
  HASH_TABLE prefixes;
  STRING_POOL pool;
  STRING_POOL entityValuePool;
  XML_Bool keepProcessing;
  XML_Bool hasParamEntityRefs;
  XML_Bool standalone;
#if defined(XML_DTD)
  XML_Bool paramEntityRead;
  HASH_TABLE paramEntities;
#endif
  PREFIX defaultPrefix;
  XML_Bool in_eldecl;
  CONTENT_SCAFFOLD *scaffold;
  unsigned contentStringLen;
  unsigned scaffSize;
  unsigned scaffCount;
  int scaffLevel;
  int *scaffIndex;
  size_t scaffIndexSize;
} DTD;

enum EntityType {
  ENTITY_INTERNAL,
  ENTITY_ATTRIBUTE,
  ENTITY_VALUE,
};

typedef struct open_internal_entity {
  const char *internalEventPtr;
  const char *internalEventEndPtr;
  struct open_internal_entity *next;
  ENTITY *entity;
  int startTagLevel;
  XML_Bool betweenDecl; 
  enum EntityType type;
} OPEN_INTERNAL_ENTITY;

enum XML_Account {
  XML_ACCOUNT_DIRECT,           
  XML_ACCOUNT_ENTITY_EXPANSION, 
  XML_ACCOUNT_NONE              
};

#if XML_GE == 1
typedef unsigned long long XmlBigCount;
typedef struct accounting {
  XmlBigCount countBytesDirect;
  XmlBigCount countBytesIndirect;
  float maximumAmplificationFactor; 
  unsigned long long activationThresholdBytes;
} ACCOUNTING;

typedef struct MALLOC_TRACKER {
  XmlBigCount bytesAllocated;
  float maximumAmplificationFactor; 
  XmlBigCount activationThresholdBytes;
} MALLOC_TRACKER;

typedef struct entity_stats {
} ENTITY_STATS;
#endif

typedef enum XML_Error PTRCALL Processor(XML_Parser parser, const char *start,
                                         const char *end, const char **endPtr);

static Processor prologProcessor;
static Processor prologInitProcessor;
static Processor contentProcessor;
static Processor cdataSectionProcessor;
#if defined(XML_DTD)
static Processor ignoreSectionProcessor;
static Processor externalParEntProcessor;
static Processor externalParEntInitProcessor;
static Processor entityValueProcessor;
static Processor entityValueInitProcessor;
#endif
static Processor epilogProcessor;
static Processor errorProcessor;
static Processor externalEntityInitProcessor;
static Processor externalEntityInitProcessor2;
static Processor externalEntityInitProcessor3;
static Processor externalEntityContentProcessor;
static Processor internalEntityProcessor;

static enum XML_Error handleUnknownEncoding(XML_Parser parser,
                                            const XML_Char *encodingName);
static enum XML_Error processXmlDecl(XML_Parser parser, int isGeneralTextEntity,
                                     const char *s, const char *next);
static enum XML_Error initializeEncoding(XML_Parser parser);
static enum XML_Error doProlog(XML_Parser parser, const ENCODING *enc,
                               const char *s, const char *end, int tok,
                               const char *next, const char **nextPtr,
                               XML_Bool haveMore, XML_Bool allowClosingDoctype,
                               enum XML_Account account);
static enum XML_Error processEntity(XML_Parser parser, ENTITY *entity,
                                    XML_Bool betweenDecl, enum EntityType type);
static enum XML_Error doContentInternal(XML_Parser parser, int startTagLevel,
                                        const ENCODING *enc, const char *start,
                                        const char *end, const char **endPtr,
                                        XML_Bool haveMore,
                                        enum XML_Account account);
static enum XML_Error doContent(XML_Parser parser, int startTagLevel,
                                const ENCODING *enc, const char *start,
                                const char *end, const char **endPtr,
                                XML_Bool haveMore, enum XML_Account account);
static enum XML_Error doCdataSection(XML_Parser parser, const ENCODING *enc,
                                     const char **startPtr, const char *end,
                                     const char **nextPtr, XML_Bool haveMore,
                                     enum XML_Account account);
#if defined(XML_DTD)
static enum XML_Error doIgnoreSection(XML_Parser parser, const ENCODING *enc,
                                      const char **startPtr, const char *end,
                                      const char **nextPtr, XML_Bool haveMore);
#endif

static void freeBindings(XML_Parser parser, BINDING *bindings);
static enum XML_Error storeAtts(XML_Parser parser, const ENCODING *enc,
                                const char *attStr, TAG_NAME *tagNamePtr,
                                BINDING **bindingsPtr,
                                enum XML_Account account);
static enum XML_Error addBinding(XML_Parser parser, PREFIX *prefix,
                                 const ATTRIBUTE_ID *attId, const XML_Char *uri,
                                 BINDING **bindingsPtr);
static int defineAttribute(ELEMENT_TYPE *type, ATTRIBUTE_ID *attId,
                           XML_Bool isCdata, XML_Bool isId,
                           const XML_Char *value, XML_Parser parser);
static enum XML_Error storeAttributeValue(XML_Parser parser,
                                          const ENCODING *enc, XML_Bool isCdata,
                                          const char *ptr, const char *end,
                                          STRING_POOL *pool,
                                          enum XML_Account account);
static enum XML_Error
appendAttributeValue(XML_Parser parser, const ENCODING *enc, XML_Bool isCdata,
                     const char *ptr, const char *end, STRING_POOL *pool,
                     enum XML_Account account, const char **nextPtr);
static ATTRIBUTE_ID *getAttributeId(XML_Parser parser, const ENCODING *enc,
                                    const char *start, const char *end);
static int setElementTypePrefix(XML_Parser parser, ELEMENT_TYPE *elementType);
#if XML_GE == 1
static enum XML_Error storeEntityValue(XML_Parser parser, const ENCODING *enc,
                                       const char *start, const char *end,
                                       enum XML_Account account,
                                       const char **nextPtr);
static enum XML_Error callStoreEntityValue(XML_Parser parser,
                                           const ENCODING *enc,
                                           const char *start, const char *end,
                                           enum XML_Account account);
#else
static enum XML_Error storeSelfEntityValue(XML_Parser parser, ENTITY *entity);
#endif
static int reportProcessingInstruction(XML_Parser parser, const ENCODING *enc,
                                       const char *start, const char *end);
static int reportComment(XML_Parser parser, const ENCODING *enc,
                         const char *start, const char *end);
static void reportDefault(XML_Parser parser, const ENCODING *enc,
                          const char *start, const char *end);

static const XML_Char *getContext(XML_Parser parser);
static XML_Bool setContext(XML_Parser parser, const XML_Char *context);

static void FASTCALL normalizePublicId(XML_Char *s);

static DTD *dtdCreate(XML_Parser parser);
static void dtdDestroy(DTD *p, XML_Bool isDocEntity, XML_Parser parser);
static int dtdCopy(XML_Parser oldParser, DTD *newDtd, const DTD *oldDtd,
                   XML_Parser parser);
static int copyEntityTable(XML_Parser oldParser, HASH_TABLE *newTable,
                           STRING_POOL *newPool, const HASH_TABLE *oldTable);
static NAMED *lookup(XML_Parser parser, HASH_TABLE *table, KEY name,
                     size_t createSize);
static void FASTCALL hashTableInit(HASH_TABLE *table, XML_Parser parser);
static void FASTCALL hashTableDestroy(HASH_TABLE *table);
static void FASTCALL hashTableIterInit(HASH_TABLE_ITER *iter,
                                       const HASH_TABLE *table);
static NAMED *FASTCALL hashTableIterNext(HASH_TABLE_ITER *iter);

static void FASTCALL poolInit(STRING_POOL *pool, XML_Parser parser);
static void FASTCALL poolClear(STRING_POOL *pool);
static void FASTCALL poolDestroy(STRING_POOL *pool);
static XML_Char *poolAppend(STRING_POOL *pool, const ENCODING *enc,
                            const char *ptr, const char *end);
static XML_Char *poolStoreString(STRING_POOL *pool, const ENCODING *enc,
                                 const char *ptr, const char *end);
static XML_Bool FASTCALL poolGrow(STRING_POOL *pool);
static bool FASTCALL poolGrowUntil(STRING_POOL *pool, size_t needed);
static const XML_Char *FASTCALL poolCopyString(STRING_POOL *pool,
                                               const XML_Char *s);
static const XML_Char *FASTCALL poolCopyStringNoFinish(STRING_POOL *pool,
                                                       const XML_Char *s);
static const XML_Char *poolCopyStringN(STRING_POOL *pool, const XML_Char *s,
                                       int n);
static const XML_Char *FASTCALL poolAppendString(STRING_POOL *pool,
                                                 const XML_Char *s);

static int FASTCALL nextScaffoldPart(XML_Parser parser);
static XML_Content *build_model(XML_Parser parser);
static ELEMENT_TYPE *getElementType(XML_Parser parser, const ENCODING *enc,
                                    const char *ptr, const char *end);

static XML_Char *copyString(const XML_Char *s, XML_Parser parser);

static struct sipkey generate_hash_secret_salt(void);
static XML_Bool startParsing(XML_Parser parser);

static XML_Parser parserCreate(const XML_Char *encodingName,
                               const XML_Memory_Handling_Suite *memsuite,
                               const XML_Char *nameSep, DTD *dtd,
                               XML_Parser parentParser);

static void parserInit(XML_Parser parser, const XML_Char *encodingName);

#if XML_GE == 1
static float accountingGetCurrentAmplification(XML_Parser rootParser);
static void accountingReportStats(XML_Parser originParser, const char *epilog);
static void accountingOnAbort(XML_Parser originParser);
static XML_Bool accountingDiffTolerated(XML_Parser originParser, int tok,
                                        const char *before, const char *after,
                                        int source_line,
                                        enum XML_Account account);

static void entityTrackingOnOpen(XML_Parser parser, ENTITY *entity,
                                 int sourceLine);
static void entityTrackingOnClose(XML_Parser parser, ENTITY *entity,
                                  int sourceLine);
#endif

static XML_Parser getRootParserOf(XML_Parser parser,
                                  unsigned int *outLevelDiff);


static bool poolAppendChar(STRING_POOL *pool, XML_Char c);

static bool poolAppendChars(STRING_POOL *pool, const XML_Char *s, size_t len);

#define poolStart(pool) ((pool)->start)
#define poolLength(pool) ((pool)->ptr - (pool)->start)
#define poolChop(pool) ((void)--(pool->ptr))
#define poolLastChar(pool) (((pool)->ptr)[-1])
#define poolDiscard(pool) ((pool)->ptr = (pool)->start)
#define poolFinish(pool) ((pool)->start = (pool)->ptr)

bool
poolAppendChar(STRING_POOL *pool, XML_Char c) {
  if (pool->ptr == pool->end && ! poolGrow(pool))
    return false;

  *(pool->ptr)++ = c;
  return true;
}

bool
poolAppendChars(STRING_POOL *pool, const XML_Char *s, size_t len) {
  if (len > SIZE_MAX / sizeof(XML_Char))
    return false;

  if (! poolGrowUntil(pool, len))
    return false;

  memcpy(pool->ptr, s, len * sizeof(XML_Char));
  pool->ptr += len;

  return true;
}

#if ! defined(XML_TESTING)
const
#endif
    XML_Bool g_reparseDeferralEnabledDefault
    = XML_TRUE; 
#if defined(XML_TESTING)
unsigned int g_bytesScanned = 0; 
#endif

struct XML_ParserStruct {
  void *m_userData;
  void *m_handlerArg;


  char *m_buffer; 
  const XML_Memory_Handling_Suite m_mem;
  const char *m_bufferPtr; 
  char *m_bufferEnd;       
  const char *m_bufferLim; 

  XML_Index m_parseEndByteIndex;
  const char *m_parseEndPtr;
  size_t m_partialTokenBytesBefore; 
  XML_Bool m_reparseDeferralEnabled;
  int m_lastBufferRequestSize;
  XML_Char *m_dataBuf;
  XML_Char *m_dataBufEnd;
  XML_StartElementHandler m_startElementHandler;
  XML_EndElementHandler m_endElementHandler;
  XML_CharacterDataHandler m_characterDataHandler;
  XML_ProcessingInstructionHandler m_processingInstructionHandler;
  XML_CommentHandler m_commentHandler;
  XML_StartCdataSectionHandler m_startCdataSectionHandler;
  XML_EndCdataSectionHandler m_endCdataSectionHandler;
  XML_DefaultHandler m_defaultHandler;
  XML_StartDoctypeDeclHandler m_startDoctypeDeclHandler;
  XML_EndDoctypeDeclHandler m_endDoctypeDeclHandler;
  XML_UnparsedEntityDeclHandler m_unparsedEntityDeclHandler;
  XML_NotationDeclHandler m_notationDeclHandler;
  XML_StartNamespaceDeclHandler m_startNamespaceDeclHandler;
  XML_EndNamespaceDeclHandler m_endNamespaceDeclHandler;
  XML_NotStandaloneHandler m_notStandaloneHandler;
  XML_ExternalEntityRefHandler m_externalEntityRefHandler;
  XML_Parser m_externalEntityRefHandlerArg;
  XML_SkippedEntityHandler m_skippedEntityHandler;
  XML_UnknownEncodingHandler m_unknownEncodingHandler;
  XML_ElementDeclHandler m_elementDeclHandler;
  XML_AttlistDeclHandler m_attlistDeclHandler;
  XML_EntityDeclHandler m_entityDeclHandler;
  XML_XmlDeclHandler m_xmlDeclHandler;
  const ENCODING *m_encoding;
  INIT_ENCODING m_initEncoding;
  const ENCODING *m_internalEncoding;
  const XML_Char *m_protocolEncodingName;
  XML_Bool m_ns;
  XML_Bool m_ns_triplets;
  void *m_unknownEncodingMem;
  void *m_unknownEncodingData;
  void *m_unknownEncodingHandlerData;
  void(XMLCALL *m_unknownEncodingRelease)(void *);
  PROLOG_STATE m_prologState;
  Processor *m_processor;
  enum XML_Error m_errorCode;
  const char *m_eventPtr;
  const char *m_eventEndPtr;
  const char *m_positionPtr;
  OPEN_INTERNAL_ENTITY *m_openInternalEntities;
  OPEN_INTERNAL_ENTITY *m_openAttributeEntities;
  OPEN_INTERNAL_ENTITY *m_openValueEntities;
  OPEN_INTERNAL_ENTITY *m_freeEntities;
  XML_Bool m_defaultExpandInternalEntities;
  int m_tagLevel;
  ENTITY *m_declEntity;
  const XML_Char *m_doctypeName;
  const XML_Char *m_doctypeSysid;
  const XML_Char *m_doctypePubid;
  const XML_Char *m_declAttributeType;
  const XML_Char *m_declNotationName;
  const XML_Char *m_declNotationPublicId;
  ELEMENT_TYPE *m_declElementType;
  ATTRIBUTE_ID *m_declAttributeId;
  XML_Bool m_declAttributeIsCdata;
  XML_Bool m_declAttributeIsId;
  DTD *m_dtd;
  const XML_Char *m_curBase;
  TAG *m_tagStack;
  TAG *m_freeTagList;
  BINDING *m_inheritedBindings;
  BINDING *m_freeBindingList;
  size_t m_attsSize;
  int m_nSpecifiedAtts;
  int m_idAttIndex;
  ATTRIBUTE *m_atts;
  NS_ATT *m_nsAtts;
  unsigned long m_nsAttsVersion;
  unsigned char m_nsAttsPower;
#if defined(XML_ATTR_INFO)
  XML_AttrInfo *m_attInfo;
#endif
  POSITION m_position;
  STRING_POOL m_tempPool;
  STRING_POOL m_temp2Pool;
  char *m_groupConnector;
  size_t m_groupSize;
  XML_Char m_namespaceSeparator;
  XML_Parser m_parentParser;
  XML_ParsingStatus m_parsingStatus;
#if defined(XML_DTD)
  XML_Bool m_isParamEntity;
  XML_Bool m_useForeignDTD;
  enum XML_ParamEntityParsing m_paramEntityParsing;
#endif
  struct sipkey m_hash_secret_salt_128;
  XML_Bool m_hash_secret_salt_set;
#if XML_GE == 1
  ACCOUNTING m_accounting;
  MALLOC_TRACKER m_alloc_tracker;
  ENTITY_STATS m_entity_stats;
#endif
  XML_Bool m_reenter;
  const XML_Char* m_mismatch;
  unsigned m_handlerCallDepth;
};

#if XML_GE == 1
#  define MALLOC(parser, s) (expat_malloc((parser), (s), __LINE__))
#  define REALLOC(parser, p, s) (expat_realloc((parser), (p), (s), __LINE__))
#  define FREE(parser, p) (expat_free((parser), (p), __LINE__))
#else
#  define MALLOC(parser, s) (parser->m_mem.malloc_fcn((s)))
#  define REALLOC(parser, p, s) (parser->m_mem.realloc_fcn((p), (s)))
#  define FREE(parser, p) (parser->m_mem.free_fcn((p)))
#endif

#if XML_GE == 1

static bool
expat_heap_increase_tolerable(XML_Parser rootParser, XmlBigCount increase,
                              int sourceLine) {
  assert(rootParser != NULL);
  assert(increase > 0);

  XmlBigCount newTotal = 0;
  bool tolerable = true;

  if ((XmlBigCount)-1 - rootParser->m_alloc_tracker.bytesAllocated < increase) {
    tolerable = false;
  } else {
    newTotal = rootParser->m_alloc_tracker.bytesAllocated + increase;

    if (newTotal >= rootParser->m_alloc_tracker.activationThresholdBytes) {
      assert(newTotal > 0);
      const float amplification
          = (float)newTotal / (float)rootParser->m_accounting.countBytesDirect;
      if (amplification
          > rootParser->m_alloc_tracker.maximumAmplificationFactor) {
        tolerable = false;
      }
    }
  }


  return tolerable;
}

#if defined(XML_TESTING)
void *
#else
static void *
#endif
expat_malloc(XML_Parser parser, size_t size, int sourceLine) {
  if (SIZE_MAX - size < sizeof(size_t) + EXPAT_MALLOC_PADDING) {
    return NULL;
  }

  const XML_Parser rootParser = getRootParserOf(parser, NULL);
  assert(rootParser->m_parentParser == NULL);

  const size_t bytesToAllocate = sizeof(size_t) + EXPAT_MALLOC_PADDING + size;

  if ((XmlBigCount)-1 - rootParser->m_alloc_tracker.bytesAllocated
      < bytesToAllocate) {
    return NULL; 
  }

  if (! expat_heap_increase_tolerable(rootParser, bytesToAllocate,
                                      sourceLine)) {
    return NULL; 
  }

  void *const mallocedPtr = parser->m_mem.malloc_fcn(bytesToAllocate);

  if (mallocedPtr == NULL) {
    return NULL;
  }

  *(size_t *)mallocedPtr = size;

  rootParser->m_alloc_tracker.bytesAllocated += bytesToAllocate;


  return (char *)mallocedPtr + sizeof(size_t) + EXPAT_MALLOC_PADDING;
}

#if defined(XML_TESTING)
void
#else
static void
#endif
expat_free(XML_Parser parser, void *ptr, int sourceLine) {
  assert(parser != NULL);

  if (ptr == NULL) {
    return;
  }

  const XML_Parser rootParser = getRootParserOf(parser, NULL);
  assert(rootParser->m_parentParser == NULL);

  void *const mallocedPtr = (char *)ptr - EXPAT_MALLOC_PADDING - sizeof(size_t);
  const size_t bytesAllocated
      = sizeof(size_t) + EXPAT_MALLOC_PADDING + *(size_t *)mallocedPtr;

  assert(rootParser->m_alloc_tracker.bytesAllocated >= bytesAllocated);
  rootParser->m_alloc_tracker.bytesAllocated -= bytesAllocated;


  parser->m_mem.free_fcn(mallocedPtr);
}

#if defined(XML_TESTING)
void *
#else
static void *
#endif
expat_realloc(XML_Parser parser, void *ptr, size_t size, int sourceLine) {
  assert(parser != NULL);

  if (ptr == NULL) {
    return expat_malloc(parser, size, sourceLine);
  }

  if (size == 0) {
    expat_free(parser, ptr, sourceLine);
    return NULL;
  }

  const XML_Parser rootParser = getRootParserOf(parser, NULL);
  assert(rootParser->m_parentParser == NULL);

  void *mallocedPtr = (char *)ptr - EXPAT_MALLOC_PADDING - sizeof(size_t);
  const size_t prevSize = *(size_t *)mallocedPtr;

  const bool isIncrease = (size > prevSize);
  const size_t absDiff
      = (size > prevSize) ? (size - prevSize) : (prevSize - size);

  if (isIncrease) {
    if (! expat_heap_increase_tolerable(rootParser, absDiff, sourceLine)) {
      return NULL; 
    }
  }

  assert(SIZE_MAX - sizeof(size_t) - EXPAT_MALLOC_PADDING >= size);

  mallocedPtr = parser->m_mem.realloc_fcn(
      mallocedPtr, sizeof(size_t) + EXPAT_MALLOC_PADDING + size);

  if (mallocedPtr == NULL) {
    return NULL;
  }

  if (isIncrease) {
    assert((XmlBigCount)-1 - rootParser->m_alloc_tracker.bytesAllocated
           >= absDiff);
    rootParser->m_alloc_tracker.bytesAllocated += absDiff;
  } else { 
    assert(rootParser->m_alloc_tracker.bytesAllocated >= absDiff);
    rootParser->m_alloc_tracker.bytesAllocated -= absDiff;
  }


  *(size_t *)mallocedPtr = size;

  return (char *)mallocedPtr + sizeof(size_t) + EXPAT_MALLOC_PADDING;
}
#endif


static const XML_Char implicitContext[]
    = {ASCII_x,     ASCII_m,     ASCII_l,      ASCII_EQUALS, ASCII_h,
       ASCII_t,     ASCII_t,     ASCII_p,      ASCII_COLON,  ASCII_SLASH,
       ASCII_SLASH, ASCII_w,     ASCII_w,      ASCII_w,      ASCII_PERIOD,
       ASCII_w,     ASCII_3,     ASCII_PERIOD, ASCII_o,      ASCII_r,
       ASCII_g,     ASCII_SLASH, ASCII_X,      ASCII_M,      ASCII_L,
       ASCII_SLASH, ASCII_1,     ASCII_9,      ASCII_9,      ASCII_8,
       ASCII_SLASH, ASCII_n,     ASCII_a,      ASCII_m,      ASCII_e,
       ASCII_s,     ASCII_p,     ASCII_a,      ASCII_c,      ASCII_e,
       '\0'};


static struct sipkey
ENTROPY_DEBUG(const char *label, struct sipkey entropy_128) {
  (void)label;
  return entropy_128;
}

static struct sipkey
generate_hash_secret_salt(void) {
  abort();
}

static void
beforeHandler(XML_Parser parser) {
  assert(parser->m_handlerCallDepth < UINT_MAX);
  parser->m_handlerCallDepth++;
}

static void
afterHandler(XML_Parser parser) {
  assert(parser->m_handlerCallDepth > 0);
  parser->m_handlerCallDepth--;
}

static bool
isCalledFromInsideHandler(XML_Parser parser) {
  return parser->m_handlerCallDepth > 0;
}

static enum XML_Error
callProcessor(XML_Parser parser, const char *start, const char *end,
              const char **endPtr) {
  const size_t have_now = EXPAT_SAFE_PTR_DIFF(end, start);

  if (parser->m_reparseDeferralEnabled
      && ! parser->m_parsingStatus.finalBuffer) {
    const size_t had_before = parser->m_partialTokenBytesBefore;
    size_t available_buffer
        = EXPAT_SAFE_PTR_DIFF(parser->m_bufferPtr, parser->m_buffer);
#if XML_CONTEXT_BYTES > 0
    available_buffer -= EXPAT_MIN(available_buffer, XML_CONTEXT_BYTES);
#endif
    available_buffer
        += EXPAT_SAFE_PTR_DIFF(parser->m_bufferLim, parser->m_bufferEnd);
    const bool enough
        = (have_now >= 2 * had_before)
          || ((size_t)parser->m_lastBufferRequestSize > available_buffer);

    if (! enough) {
      *endPtr = start; 
      return XML_ERROR_NONE;
    }
  }
#if defined(XML_TESTING)
  g_bytesScanned += (unsigned)have_now;
#endif
  enum XML_Error ret;
  *endPtr = start;
  while (1) {
    ret = parser->m_processor(parser, *endPtr, end, endPtr);

    if (parser->m_parsingStatus.parsing != XML_PARSING) {
      parser->m_reenter = XML_FALSE;
    }

    if (! parser->m_reenter) {
      break;
    }

    parser->m_reenter = XML_FALSE;
    if (ret != XML_ERROR_NONE)
      return ret;
  }

  if (ret == XML_ERROR_NONE) {
    if (*endPtr == start) {
      parser->m_partialTokenBytesBefore = have_now;
    } else {
      parser->m_partialTokenBytesBefore = 0;
    }
  }
  return ret;
}

static XML_Bool 
startParsing(XML_Parser parser) {
  if (parser->m_hash_secret_salt_set != XML_TRUE) {
    parser->m_hash_secret_salt_128 = generate_hash_secret_salt();
    parser->m_hash_secret_salt_set = XML_TRUE;
  }
  if (parser->m_ns) {
    return setContext(parser, implicitContext);
  }
  return XML_TRUE;
}

XML_Parser XMLCALL
XML_ParserCreate_MM(const XML_Char *encodingName,
                    const XML_Memory_Handling_Suite *memsuite,
                    const XML_Char *nameSep) {
  return parserCreate(encodingName, memsuite, nameSep, NULL, NULL);
}

static XML_Parser
parserCreate(const XML_Char *encodingName,
             const XML_Memory_Handling_Suite *memsuite, const XML_Char *nameSep,
             DTD *dtd, XML_Parser parentParser) {
  XML_Parser parser = NULL;

#if XML_GE == 1
  const size_t increase
      = sizeof(size_t) + EXPAT_MALLOC_PADDING + sizeof(struct XML_ParserStruct);

  if (parentParser != NULL) {
    const XML_Parser rootParser = getRootParserOf(parentParser, NULL);
    if (! expat_heap_increase_tolerable(rootParser, increase, __LINE__)) {
      return NULL;
    }
  }
#else
  UNUSED_P(parentParser);
#endif

  if (memsuite) {
    XML_Memory_Handling_Suite *mtemp;
#if XML_GE == 1
    void *const sizeAndParser
        = memsuite->malloc_fcn(sizeof(size_t) + EXPAT_MALLOC_PADDING
                               + sizeof(struct XML_ParserStruct));
    if (sizeAndParser != NULL) {
      *(size_t *)sizeAndParser = sizeof(struct XML_ParserStruct);
      parser = (XML_Parser)((char *)sizeAndParser + sizeof(size_t)
                            + EXPAT_MALLOC_PADDING);
#else
    parser = memsuite->malloc_fcn(sizeof(struct XML_ParserStruct));
    if (parser != NULL) {
#endif
      mtemp = (XML_Memory_Handling_Suite *)&(parser->m_mem);
      mtemp->malloc_fcn = memsuite->malloc_fcn;
      mtemp->realloc_fcn = memsuite->realloc_fcn;
      mtemp->free_fcn = memsuite->free_fcn;
    }
  } else {
    XML_Memory_Handling_Suite *mtemp;
#if XML_GE == 1
    void *const sizeAndParser = malloc(sizeof(size_t) + EXPAT_MALLOC_PADDING
                                       + sizeof(struct XML_ParserStruct));
    if (sizeAndParser != NULL) {
      *(size_t *)sizeAndParser = sizeof(struct XML_ParserStruct);
      parser = (XML_Parser)((char *)sizeAndParser + sizeof(size_t)
                            + EXPAT_MALLOC_PADDING);
#else
    parser = malloc(sizeof(struct XML_ParserStruct));
    if (parser != NULL) {
#endif
      mtemp = (XML_Memory_Handling_Suite *)&(parser->m_mem);
      mtemp->malloc_fcn = malloc;
      mtemp->realloc_fcn = realloc;
      mtemp->free_fcn = free;
    }
  } // cppcheck-suppress[memleak symbolName=sizeAndParser] // Cppcheck >=2.18.0

  if (! parser)
    return parser;

#if XML_GE == 1
  memset(&parser->m_alloc_tracker, 0, sizeof(MALLOC_TRACKER));
  if (parentParser == NULL) {
    parser->m_alloc_tracker.maximumAmplificationFactor
        = EXPAT_ALLOC_TRACKER_MAXIMUM_AMPLIFICATION_DEFAULT;
    parser->m_alloc_tracker.activationThresholdBytes
        = EXPAT_ALLOC_TRACKER_ACTIVATION_THRESHOLD_DEFAULT;

    parser->m_parentParser = NULL;
    parser->m_accounting.countBytesDirect = 0;
  } else {
    parser->m_parentParser = parentParser;
  }

  const XML_Parser rootParser = getRootParserOf(parser, NULL);
  assert(rootParser->m_parentParser == NULL);
  assert(SIZE_MAX - rootParser->m_alloc_tracker.bytesAllocated >= increase);
  rootParser->m_alloc_tracker.bytesAllocated += increase;

#else
  parser->m_parentParser = NULL;
#endif

  parser->m_buffer = NULL;
  parser->m_bufferLim = NULL;

  parser->m_attsSize = INIT_ATTS_SIZE;
  parser->m_atts = MALLOC(parser, parser->m_attsSize * sizeof(ATTRIBUTE));
  if (parser->m_atts == NULL) {
    FREE(parser, parser);
    return NULL;
  }
#if defined(XML_ATTR_INFO)
  parser->m_attInfo = MALLOC(parser, parser->m_attsSize * sizeof(XML_AttrInfo));
  if (parser->m_attInfo == NULL) {
    FREE(parser, parser->m_atts);
    FREE(parser, parser);
    return NULL;
  }
#endif
  parser->m_dataBuf = MALLOC(parser, INIT_DATA_BUF_SIZE * sizeof(XML_Char));
  if (parser->m_dataBuf == NULL) {
    FREE(parser, parser->m_atts);
#if defined(XML_ATTR_INFO)
    FREE(parser, parser->m_attInfo);
#endif
    FREE(parser, parser);
    return NULL;
  }
  parser->m_dataBufEnd = parser->m_dataBuf + INIT_DATA_BUF_SIZE;

  if (dtd)
    parser->m_dtd = dtd;
  else {
    parser->m_dtd = dtdCreate(parser);
    if (parser->m_dtd == NULL) {
      FREE(parser, parser->m_dataBuf);
      FREE(parser, parser->m_atts);
#if defined(XML_ATTR_INFO)
      FREE(parser, parser->m_attInfo);
#endif
      FREE(parser, parser);
      return NULL;
    }
  }

  parser->m_freeBindingList = NULL;
  parser->m_freeTagList = NULL;
  parser->m_freeEntities = NULL;

  parser->m_groupSize = 0;
  parser->m_groupConnector = NULL;

  parser->m_unknownEncodingHandler = NULL;
  parser->m_unknownEncodingHandlerData = NULL;

  parser->m_namespaceSeparator = ASCII_EXCL;
  parser->m_ns = XML_FALSE;
  parser->m_ns_triplets = XML_FALSE;

  parser->m_nsAtts = NULL;
  parser->m_nsAttsVersion = 0;
  parser->m_nsAttsPower = 0;

  parser->m_protocolEncodingName = NULL;

  poolInit(&parser->m_tempPool, parser);
  poolInit(&parser->m_temp2Pool, parser);
  parserInit(parser, encodingName);

  if (encodingName && ! parser->m_protocolEncodingName) {
    if (dtd) {
      parser->m_dtd = NULL;
    }
    XML_ParserFree(parser);
    return NULL;
  }

  if (nameSep) {
    parser->m_ns = XML_TRUE;
    parser->m_internalEncoding = XmlGetInternalEncodingNS();
    parser->m_namespaceSeparator = *nameSep;
  } else {
    parser->m_internalEncoding = XmlGetInternalEncoding();
  }

  parser->m_mismatch = NULL;

  return parser;
}

static void
parserInit(XML_Parser parser, const XML_Char *encodingName) {
  parser->m_processor = prologInitProcessor;
  XmlPrologStateInit(&parser->m_prologState);
  if (encodingName != NULL) {
    parser->m_protocolEncodingName = copyString(encodingName, parser);
  }
  parser->m_curBase = NULL;
  XmlInitEncoding(&parser->m_initEncoding, &parser->m_encoding, 0);
  parser->m_userData = NULL;
  parser->m_handlerArg = NULL;
  parser->m_startElementHandler = NULL;
  parser->m_endElementHandler = NULL;
  parser->m_characterDataHandler = NULL;
  parser->m_processingInstructionHandler = NULL;
  parser->m_commentHandler = NULL;
  parser->m_startCdataSectionHandler = NULL;
  parser->m_endCdataSectionHandler = NULL;
  parser->m_defaultHandler = NULL;
  parser->m_startDoctypeDeclHandler = NULL;
  parser->m_endDoctypeDeclHandler = NULL;
  parser->m_unparsedEntityDeclHandler = NULL;
  parser->m_notationDeclHandler = NULL;
  parser->m_startNamespaceDeclHandler = NULL;
  parser->m_endNamespaceDeclHandler = NULL;
  parser->m_notStandaloneHandler = NULL;
  parser->m_externalEntityRefHandler = NULL;
  parser->m_externalEntityRefHandlerArg = parser;
  parser->m_skippedEntityHandler = NULL;
  parser->m_elementDeclHandler = NULL;
  parser->m_attlistDeclHandler = NULL;
  parser->m_entityDeclHandler = NULL;
  parser->m_xmlDeclHandler = NULL;
  parser->m_bufferPtr = parser->m_buffer;
  parser->m_bufferEnd = parser->m_buffer;
  parser->m_parseEndByteIndex = 0;
  parser->m_parseEndPtr = NULL;
  parser->m_partialTokenBytesBefore = 0;
  parser->m_reparseDeferralEnabled = g_reparseDeferralEnabledDefault;
  parser->m_lastBufferRequestSize = 0;
  parser->m_declElementType = NULL;
  parser->m_declAttributeId = NULL;
  parser->m_declEntity = NULL;
  parser->m_doctypeName = NULL;
  parser->m_doctypeSysid = NULL;
  parser->m_doctypePubid = NULL;
  parser->m_declAttributeType = NULL;
  parser->m_declNotationName = NULL;
  parser->m_declNotationPublicId = NULL;
  parser->m_declAttributeIsCdata = XML_FALSE;
  parser->m_declAttributeIsId = XML_FALSE;
  memset(&parser->m_position, 0, sizeof(POSITION));
  parser->m_errorCode = XML_ERROR_NONE;
  parser->m_eventPtr = NULL;
  parser->m_eventEndPtr = NULL;
  parser->m_positionPtr = NULL;
  parser->m_openInternalEntities = NULL;
  parser->m_openAttributeEntities = NULL;
  parser->m_openValueEntities = NULL;
  parser->m_defaultExpandInternalEntities = XML_TRUE;
  parser->m_tagLevel = 0;
  parser->m_tagStack = NULL;
  parser->m_inheritedBindings = NULL;
  parser->m_nSpecifiedAtts = 0;
  parser->m_unknownEncodingMem = NULL;
  parser->m_unknownEncodingRelease = NULL;
  parser->m_unknownEncodingData = NULL;
  parser->m_parsingStatus.parsing = XML_INITIALIZED;
  parser->m_reenter = XML_FALSE;
  parser->m_handlerCallDepth = 0;
#if defined(XML_DTD)
  parser->m_isParamEntity = XML_FALSE;
  parser->m_useForeignDTD = XML_FALSE;
  parser->m_paramEntityParsing = XML_PARAM_ENTITY_PARSING_NEVER;
#endif
  parser->m_hash_secret_salt_128.k[0] = 0;
  parser->m_hash_secret_salt_128.k[1] = 0;
  parser->m_hash_secret_salt_set = XML_FALSE;

#if XML_GE == 1
  memset(&parser->m_accounting, 0, sizeof(ACCOUNTING));
  parser->m_accounting.maximumAmplificationFactor
      = EXPAT_BILLION_LAUGHS_ATTACK_PROTECTION_MAXIMUM_AMPLIFICATION_DEFAULT;
  parser->m_accounting.activationThresholdBytes
      = EXPAT_BILLION_LAUGHS_ATTACK_PROTECTION_ACTIVATION_THRESHOLD_DEFAULT;

  memset(&parser->m_entity_stats, 0, sizeof(ENTITY_STATS));
#endif
}


static XML_Bool
parserBusy(XML_Parser parser) {
  switch (parser->m_parsingStatus.parsing) {
  case XML_PARSING:
  case XML_SUSPENDED:
    return XML_TRUE;
  case XML_INITIALIZED:
  case XML_FINISHED:
  default:
    return XML_FALSE;
  }
}


XML_Parser XMLCALL
XML_ExternalEntityParserCreate(XML_Parser oldParser, const XML_Char *context,
                               const XML_Char *encodingName) {
  XML_Parser parser = oldParser;
  DTD *newDtd = NULL;
  DTD *oldDtd;
  XML_StartElementHandler oldStartElementHandler;
  XML_EndElementHandler oldEndElementHandler;
  XML_CharacterDataHandler oldCharacterDataHandler;
  XML_ProcessingInstructionHandler oldProcessingInstructionHandler;
  XML_CommentHandler oldCommentHandler;
  XML_StartCdataSectionHandler oldStartCdataSectionHandler;
  XML_EndCdataSectionHandler oldEndCdataSectionHandler;
  XML_DefaultHandler oldDefaultHandler;
  XML_UnparsedEntityDeclHandler oldUnparsedEntityDeclHandler;
  XML_NotationDeclHandler oldNotationDeclHandler;
  XML_StartNamespaceDeclHandler oldStartNamespaceDeclHandler;
  XML_EndNamespaceDeclHandler oldEndNamespaceDeclHandler;
  XML_NotStandaloneHandler oldNotStandaloneHandler;
  XML_ExternalEntityRefHandler oldExternalEntityRefHandler;
  XML_SkippedEntityHandler oldSkippedEntityHandler;
  XML_UnknownEncodingHandler oldUnknownEncodingHandler;
  void *oldUnknownEncodingHandlerData;
  XML_ElementDeclHandler oldElementDeclHandler;
  XML_AttlistDeclHandler oldAttlistDeclHandler;
  XML_EntityDeclHandler oldEntityDeclHandler;
  XML_XmlDeclHandler oldXmlDeclHandler;
  ELEMENT_TYPE *oldDeclElementType;

  void *oldUserData;
  void *oldHandlerArg;
  XML_Bool oldDefaultExpandInternalEntities;
  XML_Parser oldExternalEntityRefHandlerArg;
#if defined(XML_DTD)
  enum XML_ParamEntityParsing oldParamEntityParsing;
  int oldInEntityValue;
#endif
  XML_Bool oldns_triplets;
  struct sipkey oldhash_secret_salt_128;
  XML_Bool oldhash_secret_salt_set;
  XML_Bool oldReparseDeferralEnabled;

  if (oldParser == NULL)
    return NULL;

  oldDtd = parser->m_dtd;
  oldStartElementHandler = parser->m_startElementHandler;
  oldEndElementHandler = parser->m_endElementHandler;
  oldCharacterDataHandler = parser->m_characterDataHandler;
  oldProcessingInstructionHandler = parser->m_processingInstructionHandler;
  oldCommentHandler = parser->m_commentHandler;
  oldStartCdataSectionHandler = parser->m_startCdataSectionHandler;
  oldEndCdataSectionHandler = parser->m_endCdataSectionHandler;
  oldDefaultHandler = parser->m_defaultHandler;
  oldUnparsedEntityDeclHandler = parser->m_unparsedEntityDeclHandler;
  oldNotationDeclHandler = parser->m_notationDeclHandler;
  oldStartNamespaceDeclHandler = parser->m_startNamespaceDeclHandler;
  oldEndNamespaceDeclHandler = parser->m_endNamespaceDeclHandler;
  oldNotStandaloneHandler = parser->m_notStandaloneHandler;
  oldExternalEntityRefHandler = parser->m_externalEntityRefHandler;
  oldSkippedEntityHandler = parser->m_skippedEntityHandler;
  oldUnknownEncodingHandler = parser->m_unknownEncodingHandler;
  oldUnknownEncodingHandlerData = parser->m_unknownEncodingHandlerData;
  oldElementDeclHandler = parser->m_elementDeclHandler;
  oldAttlistDeclHandler = parser->m_attlistDeclHandler;
  oldEntityDeclHandler = parser->m_entityDeclHandler;
  oldXmlDeclHandler = parser->m_xmlDeclHandler;
  oldDeclElementType = parser->m_declElementType;

  oldUserData = parser->m_userData;
  oldHandlerArg = parser->m_handlerArg;
  oldDefaultExpandInternalEntities = parser->m_defaultExpandInternalEntities;
  oldExternalEntityRefHandlerArg = parser->m_externalEntityRefHandlerArg;
#if defined(XML_DTD)
  oldParamEntityParsing = parser->m_paramEntityParsing;
  oldInEntityValue = parser->m_prologState.inEntityValue;
#endif
  oldns_triplets = parser->m_ns_triplets;
  oldhash_secret_salt_128 = parser->m_hash_secret_salt_128;
  oldhash_secret_salt_set = parser->m_hash_secret_salt_set;
  oldReparseDeferralEnabled = parser->m_reparseDeferralEnabled;

#if defined(XML_DTD)
  if (! context)
    newDtd = oldDtd;
#endif

  if (parser->m_ns) {
    XML_Char tmp[2] = {parser->m_namespaceSeparator, 0};
    parser = parserCreate(encodingName, &parser->m_mem, tmp, newDtd, oldParser);
  } else {
    parser
        = parserCreate(encodingName, &parser->m_mem, NULL, newDtd, oldParser);
  }

  if (! parser)
    return NULL;

  parser->m_startElementHandler = oldStartElementHandler;
  parser->m_endElementHandler = oldEndElementHandler;
  parser->m_characterDataHandler = oldCharacterDataHandler;
  parser->m_processingInstructionHandler = oldProcessingInstructionHandler;
  parser->m_commentHandler = oldCommentHandler;
  parser->m_startCdataSectionHandler = oldStartCdataSectionHandler;
  parser->m_endCdataSectionHandler = oldEndCdataSectionHandler;
  parser->m_defaultHandler = oldDefaultHandler;
  parser->m_unparsedEntityDeclHandler = oldUnparsedEntityDeclHandler;
  parser->m_notationDeclHandler = oldNotationDeclHandler;
  parser->m_startNamespaceDeclHandler = oldStartNamespaceDeclHandler;
  parser->m_endNamespaceDeclHandler = oldEndNamespaceDeclHandler;
  parser->m_notStandaloneHandler = oldNotStandaloneHandler;
  parser->m_externalEntityRefHandler = oldExternalEntityRefHandler;
  parser->m_skippedEntityHandler = oldSkippedEntityHandler;
  parser->m_unknownEncodingHandler = oldUnknownEncodingHandler;
  parser->m_unknownEncodingHandlerData = oldUnknownEncodingHandlerData;
  parser->m_elementDeclHandler = oldElementDeclHandler;
  parser->m_attlistDeclHandler = oldAttlistDeclHandler;
  parser->m_entityDeclHandler = oldEntityDeclHandler;
  parser->m_xmlDeclHandler = oldXmlDeclHandler;
  parser->m_declElementType = oldDeclElementType;
  parser->m_userData = oldUserData;
  if (oldUserData == oldHandlerArg)
    parser->m_handlerArg = parser->m_userData;
  else
    parser->m_handlerArg = parser;
  if (oldExternalEntityRefHandlerArg != oldParser)
    parser->m_externalEntityRefHandlerArg = oldExternalEntityRefHandlerArg;
  parser->m_defaultExpandInternalEntities = oldDefaultExpandInternalEntities;
  parser->m_ns_triplets = oldns_triplets;
  parser->m_hash_secret_salt_128 = oldhash_secret_salt_128;
  parser->m_hash_secret_salt_set = oldhash_secret_salt_set;
  parser->m_reparseDeferralEnabled = oldReparseDeferralEnabled;
  parser->m_parentParser = oldParser;
#if defined(XML_DTD)
  parser->m_paramEntityParsing = oldParamEntityParsing;
  parser->m_prologState.inEntityValue = oldInEntityValue;
  if (context) {
#endif
    if (! dtdCopy(oldParser, parser->m_dtd, oldDtd, parser)
        || ! setContext(parser, context)) {
      XML_ParserFree(parser);
      return NULL;
    }
    parser->m_processor = externalEntityInitProcessor;
#if defined(XML_DTD)
  } else {
    parser->m_isParamEntity = XML_TRUE;
    XmlPrologStateInitExternalEntity(&parser->m_prologState);
    parser->m_processor = externalParEntInitProcessor;
  }
#endif
  return parser;
}

static void FASTCALL
destroyBindings(BINDING *bindings, XML_Parser parser) {
  for (;;) {
    BINDING *b = bindings;
    if (! b)
      break;
    bindings = b->nextTagBinding;
    FREE(parser, b->uri);
    FREE(parser, b);
  }
}

void XMLCALL
XML_ParserFree(XML_Parser parser) {
  TAG *tagList;
  if ((parser == NULL) || isCalledFromInsideHandler(parser))
    return;
  tagList = parser->m_tagStack;
  for (;;) {
    TAG *p;
    if (tagList == NULL) {
      if (parser->m_freeTagList == NULL)
        break;
      tagList = parser->m_freeTagList;
      parser->m_freeTagList = NULL;
    }
    p = tagList;
    tagList = tagList->parent;
    FREE(parser, p->buf.raw);
    destroyBindings(p->bindings, parser);
    FREE(parser, p);
  }
  for (OPEN_INTERNAL_ENTITY *entityList = parser->m_openInternalEntities;
       entityList != NULL;) {
    OPEN_INTERNAL_ENTITY *const openEntity = entityList;
    entityList = entityList->next;
    FREE(parser, openEntity);
  }
  for (OPEN_INTERNAL_ENTITY *entityList = parser->m_openAttributeEntities;
       entityList != NULL;) {
    OPEN_INTERNAL_ENTITY *const openEntity = entityList;
    entityList = entityList->next;
    FREE(parser, openEntity);
  }
  for (OPEN_INTERNAL_ENTITY *entityList = parser->m_openValueEntities;
       entityList != NULL;) {
    OPEN_INTERNAL_ENTITY *const openEntity = entityList;
    entityList = entityList->next;
    FREE(parser, openEntity);
  }
  for (OPEN_INTERNAL_ENTITY *entityList = parser->m_freeEntities;
       entityList != NULL;) {
    OPEN_INTERNAL_ENTITY *const openEntity = entityList;
    entityList = entityList->next;
    FREE(parser, openEntity);
  }
  parser->m_freeEntities = NULL;
  destroyBindings(parser->m_freeBindingList, parser);
  destroyBindings(parser->m_inheritedBindings, parser);
  poolDestroy(&parser->m_tempPool);
  poolDestroy(&parser->m_temp2Pool);
  FREE(parser, (void *)parser->m_protocolEncodingName);
#if defined(XML_DTD)
  if (! parser->m_isParamEntity && parser->m_dtd)
#else
  if (parser->m_dtd)
#endif
    dtdDestroy(parser->m_dtd, (XML_Bool)! parser->m_parentParser, parser);
  FREE(parser, parser->m_atts);
#if defined(XML_ATTR_INFO)
  FREE(parser, parser->m_attInfo);
#endif
  FREE(parser, parser->m_groupConnector);
  parser->m_mem.free_fcn(parser->m_buffer);
  FREE(parser, parser->m_dataBuf);
  FREE(parser, parser->m_nsAtts);
  FREE(parser, parser->m_unknownEncodingMem);
  if (parser->m_unknownEncodingRelease)
    parser->m_unknownEncodingRelease(parser->m_unknownEncodingData);
  FREE(parser, parser);
}

void XMLCALL
XML_UseParserAsHandlerArg(XML_Parser parser) {
  if (parser != NULL)
    parser->m_handlerArg = parser;
}


void XMLCALL
XML_SetReturnNSTriplet(XML_Parser parser, int do_nst) {
  if (parser == NULL)
    return;
  if (parserBusy(parser))
    return;
  parser->m_ns_triplets = do_nst ? XML_TRUE : XML_FALSE;
}

void XMLCALL
XML_SetUserData(XML_Parser parser, void *p) {
  if (parser == NULL)
    return;
  if (parser->m_handlerArg == parser->m_userData)
    parser->m_handlerArg = parser->m_userData = p;
  else
    parser->m_userData = p;
}

enum XML_Status XMLCALL
XML_SetBase(XML_Parser parser, const XML_Char *p) {
  if (parser == NULL)
    return XML_STATUS_ERROR;
  if (p) {
    p = poolCopyString(&parser->m_dtd->pool, p);
    if (! p)
      return XML_STATUS_ERROR;
    parser->m_curBase = p;
  } else
    parser->m_curBase = NULL;
  return XML_STATUS_OK;
}

const XML_Char *XMLCALL
XML_GetBase(XML_Parser parser) {
  if (parser == NULL)
    return NULL;
  return parser->m_curBase;
}

int XMLCALL
XML_GetSpecifiedAttributeCount(XML_Parser parser) {
  if (parser == NULL)
    return -1;
  return parser->m_nSpecifiedAtts;
}

int XMLCALL
XML_GetIdAttributeIndex(XML_Parser parser) {
  if (parser == NULL)
    return -1;
  return parser->m_idAttIndex;
}

#if defined(XML_ATTR_INFO)
const XML_AttrInfo *XMLCALL
XML_GetAttributeInfo(XML_Parser parser) {
  if (parser == NULL)
    return NULL;
  return parser->m_attInfo;
}
#endif

void XMLCALL
XML_SetElementHandler(XML_Parser parser, XML_StartElementHandler start,
                      XML_EndElementHandler end) {
  if (parser == NULL)
    return;
  parser->m_startElementHandler = start;
  parser->m_endElementHandler = end;
}


void XMLCALL
XML_SetCharacterDataHandler(XML_Parser parser,
                            XML_CharacterDataHandler handler) {
  if (parser != NULL)
    parser->m_characterDataHandler = handler;
}

void XMLCALL
XML_SetProcessingInstructionHandler(XML_Parser parser,
                                    XML_ProcessingInstructionHandler handler) {
  if (parser != NULL)
    parser->m_processingInstructionHandler = handler;
}

void XMLCALL
XML_SetCommentHandler(XML_Parser parser, XML_CommentHandler handler) {
  if (parser != NULL)
    parser->m_commentHandler = handler;
}

void XMLCALL
XML_SetCdataSectionHandler(XML_Parser parser,
                           XML_StartCdataSectionHandler start,
                           XML_EndCdataSectionHandler end) {
  if (parser == NULL)
    return;
  parser->m_startCdataSectionHandler = start;
  parser->m_endCdataSectionHandler = end;
}


void XMLCALL
XML_SetDefaultHandlerExpand(XML_Parser parser, XML_DefaultHandler handler) {
  if (parser == NULL)
    return;
  parser->m_defaultHandler = handler;
  parser->m_defaultExpandInternalEntities = XML_TRUE;
}

void XMLCALL
XML_SetDoctypeDeclHandler(XML_Parser parser, XML_StartDoctypeDeclHandler start,
                          XML_EndDoctypeDeclHandler end) {
  if (parser == NULL)
    return;
  parser->m_startDoctypeDeclHandler = start;
  parser->m_endDoctypeDeclHandler = end;
}


void XMLCALL
XML_SetUnparsedEntityDeclHandler(XML_Parser parser,
                                 XML_UnparsedEntityDeclHandler handler) {
  if (parser != NULL)
    parser->m_unparsedEntityDeclHandler = handler;
}

void XMLCALL
XML_SetNotationDeclHandler(XML_Parser parser, XML_NotationDeclHandler handler) {
  if (parser != NULL)
    parser->m_notationDeclHandler = handler;
}

void XMLCALL
XML_SetNamespaceDeclHandler(XML_Parser parser,
                            XML_StartNamespaceDeclHandler start,
                            XML_EndNamespaceDeclHandler end) {
  if (parser == NULL)
    return;
  parser->m_startNamespaceDeclHandler = start;
  parser->m_endNamespaceDeclHandler = end;
}


void XMLCALL
XML_SetExternalEntityRefHandler(XML_Parser parser,
                                XML_ExternalEntityRefHandler handler) {
  if (parser != NULL)
    parser->m_externalEntityRefHandler = handler;
}

void XMLCALL
XML_SetExternalEntityRefHandlerArg(XML_Parser parser, void *arg) {
  if (parser == NULL)
    return;
  if (arg)
    parser->m_externalEntityRefHandlerArg = (XML_Parser)arg;
  else
    parser->m_externalEntityRefHandlerArg = parser;
}


void XMLCALL
XML_SetXmlDeclHandler(XML_Parser parser, XML_XmlDeclHandler handler) {
  if (parser != NULL)
    parser->m_xmlDeclHandler = handler;
}

int XMLCALL
XML_SetParamEntityParsing(XML_Parser parser,
                          enum XML_ParamEntityParsing peParsing) {
  if (parser == NULL)
    return 0;
  if (parserBusy(parser))
    return 0;
#if defined(XML_DTD)
  parser->m_paramEntityParsing = peParsing;
  return 1;
#else
  return peParsing == XML_PARAM_ENTITY_PARSING_NEVER;
#endif
}

int XMLCALL
XML_SetHashSalt(XML_Parser parser, unsigned long hash_salt) {
  if (parser == NULL)
    return 0;

  const XML_Parser rootParser = getRootParserOf(parser, NULL);
  assert(! rootParser->m_parentParser);

  if (parserBusy(rootParser))
    return 0;

  rootParser->m_hash_secret_salt_128.k[0] = 0;
  rootParser->m_hash_secret_salt_128.k[1] = hash_salt;

  if (hash_salt != 0) { 
    rootParser->m_hash_secret_salt_set = XML_TRUE;

    if (sizeof(unsigned long) == 4)
      ENTROPY_DEBUG("explicit(4)", rootParser->m_hash_secret_salt_128);
    else
      ENTROPY_DEBUG("explicit(8)", rootParser->m_hash_secret_salt_128);
  }

  return 1;
}

XML_Bool XMLCALL
XML_SetHashSalt16Bytes(XML_Parser parser, const uint8_t entropy[16]) {
  if (parser == NULL)
    return XML_FALSE;

  if (entropy == NULL)
    return XML_FALSE;

  const XML_Parser rootParser = getRootParserOf(parser, NULL);
  assert(! rootParser->m_parentParser);

  if (parserBusy(rootParser))
    return XML_FALSE;

  sip_tokey(&(rootParser->m_hash_secret_salt_128), entropy);

  rootParser->m_hash_secret_salt_set = XML_TRUE;

  ENTROPY_DEBUG("explicit(16)", rootParser->m_hash_secret_salt_128);

  return XML_TRUE;
}

enum XML_Status XMLCALL
XML_Parse(XML_Parser parser, const char *s, int len, int isFinal) {
  if ((parser == NULL) || (len < 0) || ((s == NULL) && (len != 0))) {
    if (parser != NULL)
      parser->m_errorCode = XML_ERROR_INVALID_ARGUMENT;
    return XML_STATUS_ERROR;
  }
  if (isCalledFromInsideHandler(parser))
    return XML_STATUS_ERROR;
  switch (parser->m_parsingStatus.parsing) {
  case XML_SUSPENDED:
    parser->m_errorCode = XML_ERROR_SUSPENDED;
    return XML_STATUS_ERROR;
  case XML_FINISHED:
    parser->m_errorCode = XML_ERROR_FINISHED;
    return XML_STATUS_ERROR;
  case XML_INITIALIZED:
    if (parser->m_parentParser == NULL && ! startParsing(parser)) {
      parser->m_errorCode = XML_ERROR_NO_MEMORY;
      return XML_STATUS_ERROR;
    }
    EXPAT_FALLTHROUGH;
  default:
    parser->m_parsingStatus.parsing = XML_PARSING;
  }

#if XML_CONTEXT_BYTES == 0
  if (parser->m_bufferPtr == parser->m_bufferEnd) {
    const char *end;
    int nLeftOver;
    enum XML_Status result;
    if (len > XML_INDEX_MAX - parser->m_parseEndByteIndex) {
      parser->m_errorCode = XML_ERROR_NO_MEMORY;
      parser->m_eventPtr = parser->m_eventEndPtr = NULL;
      parser->m_processor = errorProcessor;
      return XML_STATUS_ERROR;
    }
    parser->m_lastBufferRequestSize = len;
    parser->m_parseEndByteIndex += len;
    parser->m_positionPtr = s;
    parser->m_parsingStatus.finalBuffer = (XML_Bool)isFinal;

    parser->m_errorCode
        = callProcessor(parser, s, parser->m_parseEndPtr = s + len, &end);

    if (parser->m_errorCode != XML_ERROR_NONE) {
      parser->m_eventEndPtr = parser->m_eventPtr;
      parser->m_processor = errorProcessor;
      return XML_STATUS_ERROR;
    } else {
      switch (parser->m_parsingStatus.parsing) {
      case XML_SUSPENDED:
        result = XML_STATUS_SUSPENDED;
        break;
      case XML_INITIALIZED:
      case XML_PARSING:
        if (isFinal) {
          parser->m_parsingStatus.parsing = XML_FINISHED;
          return XML_STATUS_OK;
        }
        EXPAT_FALLTHROUGH;
      default:
        result = XML_STATUS_OK;
      }
    }

    XmlUpdatePosition(parser->m_encoding, parser->m_positionPtr, end,
                      &parser->m_position);
    nLeftOver = s + len - end;
    if (nLeftOver) {
      const enum XML_Parsing originalStatus = parser->m_parsingStatus.parsing;
      parser->m_parsingStatus.parsing = XML_PARSING;
      void *const temp = XML_GetBuffer(parser, nLeftOver);
      parser->m_parsingStatus.parsing = originalStatus;
      parser->m_lastBufferRequestSize = len;
      if (temp == NULL) {
        parser->m_eventPtr = parser->m_eventEndPtr = NULL;
        parser->m_processor = errorProcessor;
        return XML_STATUS_ERROR;
      }
      memcpy(parser->m_buffer, end, nLeftOver);
    }
    parser->m_bufferPtr = parser->m_buffer;
    parser->m_bufferEnd = parser->m_buffer + nLeftOver;
    parser->m_positionPtr = parser->m_bufferPtr;
    parser->m_parseEndPtr = parser->m_bufferEnd;
    parser->m_eventPtr = parser->m_bufferPtr;
    parser->m_eventEndPtr = parser->m_bufferPtr;
    return result;
  }
#endif
  void *buff = XML_GetBuffer(parser, len);
  if (buff == NULL)
    return XML_STATUS_ERROR;
  if (len > 0) {
    assert(s != NULL); 
    memcpy(buff, s, len);
  }
  return XML_ParseBuffer(parser, len, isFinal);
}

enum XML_Status XMLCALL
XML_ParseBuffer(XML_Parser parser, int len, int isFinal) {
  const char *start;
  enum XML_Status result = XML_STATUS_OK;

  if ((parser == NULL) || isCalledFromInsideHandler(parser))
    return XML_STATUS_ERROR;

  if (len < 0) {
    parser->m_errorCode = XML_ERROR_INVALID_ARGUMENT;
    return XML_STATUS_ERROR;
  }

  switch (parser->m_parsingStatus.parsing) {
  case XML_SUSPENDED:
    parser->m_errorCode = XML_ERROR_SUSPENDED;
    return XML_STATUS_ERROR;
  case XML_FINISHED:
    parser->m_errorCode = XML_ERROR_FINISHED;
    return XML_STATUS_ERROR;
  case XML_INITIALIZED:
    if (! parser->m_bufferPtr) {
      parser->m_errorCode = XML_ERROR_NO_BUFFER;
      return XML_STATUS_ERROR;
    }

    if (parser->m_parentParser == NULL && ! startParsing(parser)) {
      parser->m_errorCode = XML_ERROR_NO_MEMORY;
      return XML_STATUS_ERROR;
    }
    EXPAT_FALLTHROUGH;
  default:
    parser->m_parsingStatus.parsing = XML_PARSING;
  }

  if (len > XML_INDEX_MAX - parser->m_parseEndByteIndex) {
    parser->m_errorCode = XML_ERROR_NO_MEMORY;
    parser->m_eventPtr = parser->m_eventEndPtr = NULL;
    parser->m_processor = errorProcessor;
    return XML_STATUS_ERROR;
  }

  start = parser->m_bufferPtr;
  parser->m_positionPtr = start;
  parser->m_bufferEnd += len;
  parser->m_parseEndPtr = parser->m_bufferEnd;
  parser->m_parseEndByteIndex += len;
  parser->m_parsingStatus.finalBuffer = (XML_Bool)isFinal;

  parser->m_errorCode = callProcessor(parser, start, parser->m_parseEndPtr,
                                      &parser->m_bufferPtr);

  if (parser->m_errorCode != XML_ERROR_NONE) {
    parser->m_eventEndPtr = parser->m_eventPtr;
    parser->m_processor = errorProcessor;
    return XML_STATUS_ERROR;
  } else {
    switch (parser->m_parsingStatus.parsing) {
    case XML_SUSPENDED:
      result = XML_STATUS_SUSPENDED;
      break;
    case XML_INITIALIZED:
    case XML_PARSING:
      if (isFinal) {
        parser->m_parsingStatus.parsing = XML_FINISHED;
        return result;
      }
      break;
    default:; 
    }
  }

  XmlUpdatePosition(parser->m_encoding, parser->m_positionPtr,
                    parser->m_bufferPtr, &parser->m_position);
  parser->m_positionPtr = parser->m_bufferPtr;
  parser->m_eventPtr = parser->m_bufferPtr;
  parser->m_eventEndPtr = parser->m_bufferPtr;
  return result;
}

static void
setParserBuffer(XML_Parser parser, char *newBuf, int newBufSize, int keep) {
  parser->m_bufferLim = newBuf + newBufSize;
  if (parser->m_bufferPtr) {
    const int parsing
        = (int)EXPAT_SAFE_PTR_DIFF(parser->m_bufferEnd, parser->m_bufferPtr);
    memcpy(newBuf, parser->m_bufferPtr - keep, parsing + keep);
    parser->m_mem.free_fcn(parser->m_buffer);
    parser->m_buffer = newBuf;
    parser->m_bufferEnd = newBuf + parsing + keep;
    parser->m_bufferPtr = newBuf + keep;
  } else {
    parser->m_buffer = newBuf;
    parser->m_bufferEnd = newBuf;
    parser->m_bufferPtr = newBuf;
  }
}

void *XMLCALL
XML_GetBuffer(XML_Parser parser, int len) {
  if ((parser == NULL) || isCalledFromInsideHandler(parser))
    return NULL;
  if (len < 0) {
    parser->m_errorCode = XML_ERROR_NO_MEMORY;
    return NULL;
  }
  switch (parser->m_parsingStatus.parsing) {
  case XML_SUSPENDED:
    parser->m_errorCode = XML_ERROR_SUSPENDED;
    return NULL;
  case XML_FINISHED:
    parser->m_errorCode = XML_ERROR_FINISHED;
    return NULL;
  default:;
  }

  parser->m_lastBufferRequestSize = len;
  if (len > EXPAT_SAFE_PTR_DIFF(parser->m_bufferLim, parser->m_bufferEnd)
      || parser->m_buffer == NULL) {
    int neededSize = (int)((unsigned)len
                           + (unsigned)EXPAT_SAFE_PTR_DIFF(
                               parser->m_bufferEnd, parser->m_bufferPtr));
    if (neededSize < 0) {
      parser->m_errorCode = XML_ERROR_NO_MEMORY;
      return NULL;
    }
#if XML_CONTEXT_BYTES > 0
    const int parsed
        = (int)EXPAT_SAFE_PTR_DIFF(parser->m_bufferPtr, parser->m_buffer);
    int keep = parsed;
    if (keep > XML_CONTEXT_BYTES)
      keep = XML_CONTEXT_BYTES;
    if (keep > INT_MAX - neededSize) {
      parser->m_errorCode = XML_ERROR_NO_MEMORY;
      return NULL;
    }
#else
    int keep = 0;
#endif
    neededSize += keep;
    if (parser->m_buffer && parser->m_bufferPtr
        && neededSize
               <= EXPAT_SAFE_PTR_DIFF(parser->m_bufferLim, parser->m_buffer)) {
#if XML_CONTEXT_BYTES > 0
      if (keep < parsed) {
        int offset = parsed - keep;
        memmove(parser->m_buffer, &parser->m_buffer[offset],
                parser->m_bufferEnd - parser->m_bufferPtr + keep);
        parser->m_bufferEnd -= offset;
        parser->m_bufferPtr -= offset;
      }
#else
      memmove(parser->m_buffer, parser->m_bufferPtr,
              EXPAT_SAFE_PTR_DIFF(parser->m_bufferEnd, parser->m_bufferPtr));
      parser->m_bufferEnd
          = parser->m_buffer
            + EXPAT_SAFE_PTR_DIFF(parser->m_bufferEnd, parser->m_bufferPtr);
      parser->m_bufferPtr = parser->m_buffer;
#endif
    } else {
      int bufferSize
          = (int)EXPAT_SAFE_PTR_DIFF(parser->m_bufferLim, parser->m_buffer);
      if (bufferSize == 0)
        bufferSize = INIT_BUFFER_SIZE;
      do {
        bufferSize = (int)(2U * (unsigned)bufferSize);
      } while (bufferSize < neededSize && bufferSize > 0);
      if (bufferSize <= 0) {
        parser->m_errorCode = XML_ERROR_NO_MEMORY;
        return NULL;
      }
      char *const newBuf = parser->m_mem.malloc_fcn(bufferSize);
      if (newBuf == NULL) {
        parser->m_errorCode = XML_ERROR_NO_MEMORY;
        return NULL;
      }
      setParserBuffer(parser, newBuf, bufferSize, keep);
    }
    parser->m_eventPtr = parser->m_eventEndPtr = NULL;
    parser->m_positionPtr = NULL;
  }
  return parser->m_bufferEnd;
}

static void
triggerReenter(XML_Parser parser) {
  parser->m_reenter = XML_TRUE;
}

enum XML_Status XMLCALL
XML_StopParser(XML_Parser parser, XML_Bool resumable) {
  if (parser == NULL)
    return XML_STATUS_ERROR;
  switch (parser->m_parsingStatus.parsing) {
  case XML_INITIALIZED:
    parser->m_errorCode = XML_ERROR_NOT_STARTED;
    return XML_STATUS_ERROR;
  case XML_SUSPENDED:
    if (resumable) {
      parser->m_errorCode = XML_ERROR_SUSPENDED;
      return XML_STATUS_ERROR;
    }
    parser->m_parsingStatus.parsing = XML_FINISHED;
    break;
  case XML_FINISHED:
    parser->m_errorCode = XML_ERROR_FINISHED;
    return XML_STATUS_ERROR;
  case XML_PARSING:
    if (resumable) {
#if defined(XML_DTD)
      if (parser->m_isParamEntity) {
        parser->m_errorCode = XML_ERROR_SUSPEND_PE;
        return XML_STATUS_ERROR;
      }
#endif
      parser->m_parsingStatus.parsing = XML_SUSPENDED;
    } else
      parser->m_parsingStatus.parsing = XML_FINISHED;
    break;
  default:
    assert(0);
  }
  return XML_STATUS_OK;
}

enum XML_Status XMLCALL
XML_ResumeParser(XML_Parser parser) {
  enum XML_Status result = XML_STATUS_OK;

  if ((parser == NULL) || isCalledFromInsideHandler(parser))
    return XML_STATUS_ERROR;
  if (parser->m_parsingStatus.parsing != XML_SUSPENDED) {
    parser->m_errorCode = XML_ERROR_NOT_SUSPENDED;
    return XML_STATUS_ERROR;
  }
  parser->m_parsingStatus.parsing = XML_PARSING;

  parser->m_errorCode = callProcessor(
      parser, parser->m_bufferPtr, parser->m_parseEndPtr, &parser->m_bufferPtr);

  if (parser->m_errorCode != XML_ERROR_NONE) {
    parser->m_eventEndPtr = parser->m_eventPtr;
    parser->m_processor = errorProcessor;
    return XML_STATUS_ERROR;
  } else {
    switch (parser->m_parsingStatus.parsing) {
    case XML_SUSPENDED:
      result = XML_STATUS_SUSPENDED;
      break;
    case XML_INITIALIZED:
    case XML_PARSING:
      if (parser->m_parsingStatus.finalBuffer) {
        parser->m_parsingStatus.parsing = XML_FINISHED;
        return result;
      }
      break;
    default:;
    }
  }

  XmlUpdatePosition(parser->m_encoding, parser->m_positionPtr,
                    parser->m_bufferPtr, &parser->m_position);
  parser->m_positionPtr = parser->m_bufferPtr;
  return result;
}

void XMLCALL
XML_GetParsingStatus(XML_Parser parser, XML_ParsingStatus *status) {
  if (parser == NULL)
    return;
  assert(status != NULL);
  *status = parser->m_parsingStatus;
}

enum XML_Error XMLCALL
XML_GetErrorCode(XML_Parser parser) {
  if (parser == NULL)
    return XML_ERROR_INVALID_ARGUMENT;
  return parser->m_errorCode;
}

XML_Index XMLCALL
XML_GetCurrentByteIndex(XML_Parser parser) {
  if (parser == NULL)
    return -1;
  if (parser->m_eventPtr)
    return (XML_Index)(parser->m_parseEndByteIndex
                       - (parser->m_parseEndPtr - parser->m_eventPtr));
  return parser->m_parseEndByteIndex;
}


XML_Size XMLCALL
XML_GetCurrentLineNumber(XML_Parser parser) {
  if (parser == NULL)
    return 0;
  if (parser->m_eventPtr && parser->m_eventPtr >= parser->m_positionPtr) {
    XmlUpdatePosition(parser->m_encoding, parser->m_positionPtr,
                      parser->m_eventPtr, &parser->m_position);
    parser->m_positionPtr = parser->m_eventPtr;
  }
  return parser->m_position.lineNumber + 1;
}

XML_Size XMLCALL
XML_GetCurrentColumnNumber(XML_Parser parser) {
  if (parser == NULL)
    return 0;
  if (parser->m_eventPtr && parser->m_eventPtr >= parser->m_positionPtr) {
    XmlUpdatePosition(parser->m_encoding, parser->m_positionPtr,
                      parser->m_eventPtr, &parser->m_position);
    parser->m_positionPtr = parser->m_eventPtr;
  }
  return parser->m_position.columnNumber;
}


#if XML_GE == 1
XML_Bool XMLCALL
XML_SetBillionLaughsAttackProtectionMaximumAmplification(
    XML_Parser parser, float maximumAmplificationFactor) {
  if ((parser == NULL) || (parser->m_parentParser != NULL)
      || isnan(maximumAmplificationFactor)
      || (maximumAmplificationFactor < 1.0f)) {
    return XML_FALSE;
  }
  parser->m_accounting.maximumAmplificationFactor = maximumAmplificationFactor;
  return XML_TRUE;
}

XML_Bool XMLCALL
XML_SetBillionLaughsAttackProtectionActivationThreshold(
    XML_Parser parser, unsigned long long activationThresholdBytes) {
  if ((parser == NULL) || (parser->m_parentParser != NULL)) {
    return XML_FALSE;
  }
  parser->m_accounting.activationThresholdBytes = activationThresholdBytes;
  return XML_TRUE;
}

XML_Bool XMLCALL
XML_SetAllocTrackerMaximumAmplification(XML_Parser parser,
                                        float maximumAmplificationFactor) {
  if ((parser == NULL) || (parser->m_parentParser != NULL)
      || isnan(maximumAmplificationFactor)
      || (maximumAmplificationFactor < 1.0f)) {
    return XML_FALSE;
  }
  parser->m_alloc_tracker.maximumAmplificationFactor
      = maximumAmplificationFactor;
  return XML_TRUE;
}

XML_Bool XMLCALL
XML_SetAllocTrackerActivationThreshold(
    XML_Parser parser, unsigned long long activationThresholdBytes) {
  if ((parser == NULL) || (parser->m_parentParser != NULL)) {
    return XML_FALSE;
  }
  parser->m_alloc_tracker.activationThresholdBytes = activationThresholdBytes;
  return XML_TRUE;
}
#endif

XML_Bool XMLCALL
XML_SetReparseDeferralEnabled(XML_Parser parser, XML_Bool enabled) {
  if (parser != NULL && (enabled == XML_TRUE || enabled == XML_FALSE)) {
    parser->m_reparseDeferralEnabled = enabled;
    return XML_TRUE;
  }
  return XML_FALSE;
}

static XML_Bool
storeRawNames(XML_Parser parser) {
  TAG *tag = parser->m_tagStack;
  while (tag) {
    size_t bufSize;
    size_t nameLen = sizeof(XML_Char) * (tag->name.strLen + 1);
    size_t rawNameLen;
    char *rawNameBuf = tag->buf.raw + nameLen;
    if (tag->rawName == rawNameBuf)
      break;
    rawNameLen = ROUND_UP(tag->rawNameLength, sizeof(XML_Char));
    if (rawNameLen > SIZE_MAX - nameLen)
      return XML_FALSE;
    bufSize = nameLen + rawNameLen;
    if (bufSize > (size_t)(tag->bufEnd - tag->buf.raw)) {
      char *temp = REALLOC(parser, tag->buf.raw, bufSize);
      if (temp == NULL)
        return XML_FALSE;
      if (tag->name.str == tag->buf.str)
        tag->name.str = (XML_Char *)temp;
      if (tag->name.localPart)
        tag->name.localPart
            = (XML_Char *)temp + (tag->name.localPart - tag->buf.str);
      tag->buf.raw = temp;
      tag->bufEnd = temp + bufSize;
      rawNameBuf = temp + nameLen;
    }
    memcpy(rawNameBuf, tag->rawName, tag->rawNameLength);
    tag->rawName = rawNameBuf;
    tag = tag->parent;
  }
  return XML_TRUE;
}

static enum XML_Error PTRCALL
contentProcessor(XML_Parser parser, const char *start, const char *end,
                 const char **endPtr) {
  enum XML_Error result = doContent(
      parser, parser->m_parentParser ? 1 : 0, parser->m_encoding, start, end,
      endPtr, (XML_Bool)! parser->m_parsingStatus.finalBuffer,
      XML_ACCOUNT_DIRECT);
  return result;
}

static enum XML_Error PTRCALL
externalEntityInitProcessor(XML_Parser parser, const char *start,
                            const char *end, const char **endPtr) {
  enum XML_Error result = initializeEncoding(parser);
  if (result != XML_ERROR_NONE)
    return result;
  parser->m_processor = externalEntityInitProcessor2;
  return externalEntityInitProcessor2(parser, start, end, endPtr);
}

static enum XML_Error PTRCALL
externalEntityInitProcessor2(XML_Parser parser, const char *start,
                             const char *end, const char **endPtr) {
  const char *next = start; 
  int tok = XmlContentTok(parser->m_encoding, start, end, &next);
  switch (tok) {
  case XML_TOK_BOM:
#if XML_GE == 1
    if (! accountingDiffTolerated(parser, tok, start, next, __LINE__,
                                  XML_ACCOUNT_DIRECT)) {
      accountingOnAbort(parser);
      return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
    }
#endif

    if (next == end && ! parser->m_parsingStatus.finalBuffer) {
      *endPtr = next;
      return XML_ERROR_NONE;
    }
    start = next;
    break;
  case XML_TOK_PARTIAL:
    if (! parser->m_parsingStatus.finalBuffer) {
      *endPtr = start;
      return XML_ERROR_NONE;
    }
    parser->m_eventPtr = start;
    return XML_ERROR_UNCLOSED_TOKEN;
  case XML_TOK_PARTIAL_CHAR:
    if (! parser->m_parsingStatus.finalBuffer) {
      *endPtr = start;
      return XML_ERROR_NONE;
    }
    parser->m_eventPtr = start;
    return XML_ERROR_PARTIAL_CHAR;
  }
  parser->m_processor = externalEntityInitProcessor3;
  return externalEntityInitProcessor3(parser, start, end, endPtr);
}

static enum XML_Error PTRCALL
externalEntityInitProcessor3(XML_Parser parser, const char *start,
                             const char *end, const char **endPtr) {
  int tok;
  const char *next = start; 
  parser->m_eventPtr = start;
  tok = XmlContentTok(parser->m_encoding, start, end, &next);
  parser->m_eventEndPtr = next;

  switch (tok) {
  case XML_TOK_XML_DECL: {
    enum XML_Error result;
    result = processXmlDecl(parser, 1, start, next);
    if (result != XML_ERROR_NONE)
      return result;
    switch (parser->m_parsingStatus.parsing) {
    case XML_SUSPENDED:
      *endPtr = next;
      return XML_ERROR_NONE;
    case XML_FINISHED:
      return XML_ERROR_ABORTED;
    case XML_PARSING:
      if (parser->m_reenter) {
        return XML_ERROR_UNEXPECTED_STATE; // LCOV_EXCL_LINE
      }
      EXPAT_FALLTHROUGH;
    default:
      start = next;
    }
  } break;
  case XML_TOK_PARTIAL:
    if (! parser->m_parsingStatus.finalBuffer) {
      *endPtr = start;
      return XML_ERROR_NONE;
    }
    return XML_ERROR_UNCLOSED_TOKEN;
  case XML_TOK_PARTIAL_CHAR:
    if (! parser->m_parsingStatus.finalBuffer) {
      *endPtr = start;
      return XML_ERROR_NONE;
    }
    return XML_ERROR_PARTIAL_CHAR;
  }
  parser->m_processor = externalEntityContentProcessor;
  parser->m_tagLevel = 1;
  return externalEntityContentProcessor(parser, start, end, endPtr);
}

static enum XML_Error PTRCALL
externalEntityContentProcessor(XML_Parser parser, const char *start,
                               const char *end, const char **endPtr) {
  enum XML_Error result
      = doContent(parser, 1, parser->m_encoding, start, end, endPtr,
                  (XML_Bool)! parser->m_parsingStatus.finalBuffer,
                  XML_ACCOUNT_ENTITY_EXPANSION);
  return result;
}

static enum XML_Error
doContent(XML_Parser parser, int startTagLevel, const ENCODING *enc,
          const char *s, const char *end, const char **nextPtr,
          XML_Bool haveMore, enum XML_Account account) {
  enum XML_Error result = doContentInternal(parser, startTagLevel, enc, s, end,
                                            nextPtr, haveMore, account);
  if (result == XML_ERROR_NONE) {
    if (! storeRawNames(parser))
      return XML_ERROR_NO_MEMORY;
  }
  return result;
}

static enum XML_Error
doContentInternal(XML_Parser parser, int startTagLevel, const ENCODING *enc,
                  const char *s, const char *end, const char **nextPtr,
                  XML_Bool haveMore, enum XML_Account account) {
  DTD *const dtd = parser->m_dtd;

  const char **eventPP;
  const char **eventEndPP;
  if (enc == parser->m_encoding) {
    eventPP = &parser->m_eventPtr;
    eventEndPP = &parser->m_eventEndPtr;
  } else {
    eventPP = &(parser->m_openInternalEntities->internalEventPtr);
    eventEndPP = &(parser->m_openInternalEntities->internalEventEndPtr);
  }
  *eventPP = s;

  for (;;) {
    const char *next = s; 
    int tok = XmlContentTok(enc, s, end, &next);
#if XML_GE == 1
    const char *accountAfter
        = ((tok == XML_TOK_TRAILING_RSQB) || (tok == XML_TOK_TRAILING_CR))
              ? (haveMore ? s  : end)
              : next;
    if (! accountingDiffTolerated(parser, tok, s, accountAfter, __LINE__,
                                  account)) {
      accountingOnAbort(parser);
      return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
    }
#endif
    *eventEndPP = next;
    switch (tok) {
    case XML_TOK_TRAILING_CR:
      if (haveMore) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      *eventEndPP = end;
      if (parser->m_characterDataHandler) {
        XML_Char c = 0xA;
        beforeHandler(parser);
        parser->m_characterDataHandler(parser->m_handlerArg, &c, 1);
        afterHandler(parser);
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, end);
      if (startTagLevel == 0)
        return XML_ERROR_NO_ELEMENTS;
      if (parser->m_tagLevel != startTagLevel)
        return XML_ERROR_ASYNC_ENTITY;
      *nextPtr = end;
      return XML_ERROR_NONE;
    case XML_TOK_NONE:
      if (haveMore) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      if (startTagLevel > 0) {
        if (parser->m_tagLevel != startTagLevel)
          return XML_ERROR_ASYNC_ENTITY;
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      return XML_ERROR_NO_ELEMENTS;
    case XML_TOK_INVALID:
      *eventPP = next;
      return XML_ERROR_INVALID_TOKEN;
    case XML_TOK_PARTIAL:
      if (haveMore) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      return XML_ERROR_UNCLOSED_TOKEN;
    case XML_TOK_PARTIAL_CHAR:
      if (haveMore) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      return XML_ERROR_PARTIAL_CHAR;
    case XML_TOK_ENTITY_REF: {
      const XML_Char *name;
      ENTITY *entity;
      XML_Char ch = (XML_Char)XmlPredefinedEntityName(
          enc, s + enc->minBytesPerChar, next - enc->minBytesPerChar);
      if (ch) {
#if XML_GE == 1
        accountingDiffTolerated(parser, tok, (char *)&ch,
                                ((char *)&ch) + sizeof(XML_Char), __LINE__,
                                XML_ACCOUNT_ENTITY_EXPANSION);
#endif
        if (parser->m_characterDataHandler) {
          beforeHandler(parser);
          parser->m_characterDataHandler(parser->m_handlerArg, &ch, 1);
          afterHandler(parser);
        } else if (parser->m_defaultHandler)
          reportDefault(parser, enc, s, next);
        break;
      }
      name = poolStoreString(&dtd->pool, enc, s + enc->minBytesPerChar,
                             next - enc->minBytesPerChar);
      if (! name)
        return XML_ERROR_NO_MEMORY;
      entity = (ENTITY *)lookup(parser, &dtd->generalEntities, name, 0);
      poolDiscard(&dtd->pool);
      if (! dtd->hasParamEntityRefs || dtd->standalone) {
        if (! entity)
          return XML_ERROR_UNDEFINED_ENTITY;
        else if (! entity->is_internal)
          return XML_ERROR_ENTITY_DECLARED_IN_PE;
      } else if (! entity) {
        if (parser->m_skippedEntityHandler) {
          beforeHandler(parser);
          parser->m_skippedEntityHandler(parser->m_handlerArg, name, 0);
          afterHandler(parser);
        }
        return XML_ERROR_UNDEFINED_ENTITY;
      }
      if (entity->open)
        return XML_ERROR_RECURSIVE_ENTITY_REF;
      if (entity->notation)
        return XML_ERROR_BINARY_ENTITY_REF;
      if (entity->textPtr) {
        enum XML_Error result;
        if (! parser->m_defaultExpandInternalEntities) {
          if (parser->m_skippedEntityHandler) {
            beforeHandler(parser);
            parser->m_skippedEntityHandler(parser->m_handlerArg, entity->name,
                                           0);
            afterHandler(parser);
          } else if (parser->m_defaultHandler)
            reportDefault(parser, enc, s, next);
          break;
        }
        result = processEntity(parser, entity, XML_FALSE, ENTITY_INTERNAL);
        if (result != XML_ERROR_NONE)
          return result;
      } else if (parser->m_externalEntityRefHandler) {
        const XML_Char *context;
        entity->open = XML_TRUE;
        context = getContext(parser);
        entity->open = XML_FALSE;
        if (! context)
          return XML_ERROR_NO_MEMORY;
        beforeHandler(parser);
        const int status = parser->m_externalEntityRefHandler(
            parser->m_externalEntityRefHandlerArg, context, entity->base,
            entity->systemId, entity->publicId);
        afterHandler(parser);
        if (! status)
          return XML_ERROR_EXTERNAL_ENTITY_HANDLING;
        poolDiscard(&parser->m_tempPool);
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
      break;
    }
    case XML_TOK_START_TAG_NO_ATTS:
    case XML_TOK_START_TAG_WITH_ATTS: {
      TAG *tag;
      enum XML_Error result;
      XML_Char *toPtr;
      if (parser->m_freeTagList) {
        tag = parser->m_freeTagList;
        parser->m_freeTagList = parser->m_freeTagList->parent;
      } else {
        tag = MALLOC(parser, sizeof(TAG));
        if (! tag)
          return XML_ERROR_NO_MEMORY;
        tag->buf.raw = MALLOC(parser, INIT_TAG_BUF_SIZE);
        if (! tag->buf.raw) {
          FREE(parser, tag);
          return XML_ERROR_NO_MEMORY;
        }
        tag->bufEnd = tag->buf.raw + INIT_TAG_BUF_SIZE;
      }
      tag->bindings = NULL;
      tag->parent = parser->m_tagStack;
      parser->m_tagStack = tag;
      tag->name.localPart = NULL;
      tag->name.prefix = NULL;
      tag->rawName = s + enc->minBytesPerChar;
      tag->rawNameLength = XmlNameLength(enc, tag->rawName);
      ++parser->m_tagLevel;
      {
        const char *rawNameEnd = tag->rawName + tag->rawNameLength;
        const char *fromPtr = tag->rawName;
        toPtr = tag->buf.str;
        for (;;) {
          const enum XML_Convert_Result convert_res
              = XmlConvert(enc, &fromPtr, rawNameEnd, (ICHAR **)&toPtr,
                           (ICHAR *)tag->bufEnd - 1);
          const size_t convLen = (size_t)(toPtr - tag->buf.str);
          if ((fromPtr >= rawNameEnd)
              || (convert_res == XML_CONVERT_INPUT_INCOMPLETE)) {
            tag->name.strLen = convLen;
            break;
          }
          if (SIZE_MAX / 2 < (size_t)(tag->bufEnd - tag->buf.raw))
            return XML_ERROR_NO_MEMORY;
          const size_t bufSize = (size_t)(tag->bufEnd - tag->buf.raw) * 2;
          {
            char *temp = REALLOC(parser, tag->buf.raw, bufSize);
            if (temp == NULL)
              return XML_ERROR_NO_MEMORY;
            tag->buf.raw = temp;
            tag->bufEnd = temp + bufSize;
            toPtr = (XML_Char *)temp + convLen;
          }
        }
      }
      tag->name.str = tag->buf.str;
      *toPtr = XML_T('\0');
      result
          = storeAtts(parser, enc, s, &(tag->name), &(tag->bindings), account);
      if (result)
        return result;
      if (parser->m_startElementHandler) {
        beforeHandler(parser);
        parser->m_startElementHandler(parser->m_handlerArg, tag->name.str,
                                      (const XML_Char **)parser->m_atts);
        afterHandler(parser);
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
      poolClear(&parser->m_tempPool);
      break;
    }
    case XML_TOK_EMPTY_ELEMENT_NO_ATTS:
    case XML_TOK_EMPTY_ELEMENT_WITH_ATTS: {
      const char *rawName = s + enc->minBytesPerChar;
      enum XML_Error result;
      BINDING *bindings = NULL;
      XML_Bool noElmHandlers = XML_TRUE;
      TAG_NAME name;
      name.str = poolStoreString(&parser->m_tempPool, enc, rawName,
                                 rawName + XmlNameLength(enc, rawName));
      if (! name.str)
        return XML_ERROR_NO_MEMORY;
      poolFinish(&parser->m_tempPool);
      result = storeAtts(parser, enc, s, &name, &bindings,
                         XML_ACCOUNT_NONE );
      if (result != XML_ERROR_NONE) {
        freeBindings(parser, bindings);
        return result;
      }
      poolFinish(&parser->m_tempPool);
      if (parser->m_startElementHandler) {
        beforeHandler(parser);
        parser->m_startElementHandler(parser->m_handlerArg, name.str,
                                      (const XML_Char **)parser->m_atts);
        afterHandler(parser);
        noElmHandlers = XML_FALSE;
      }
      if (parser->m_endElementHandler) {
        if (parser->m_startElementHandler)
          *eventPP = *eventEndPP;
        beforeHandler(parser);
        parser->m_endElementHandler(parser->m_handlerArg, name.str);
        afterHandler(parser);
        noElmHandlers = XML_FALSE;
      }
      if (noElmHandlers && parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
      poolClear(&parser->m_tempPool);
      freeBindings(parser, bindings);
    }
      if ((parser->m_tagLevel == 0)
          && (parser->m_parsingStatus.parsing != XML_FINISHED)) {
        if (parser->m_parsingStatus.parsing == XML_SUSPENDED
            || (parser->m_parsingStatus.parsing == XML_PARSING
                && parser->m_reenter))
          parser->m_processor = epilogProcessor;
        else
          return epilogProcessor(parser, next, end, nextPtr);
      }
      break;
    case XML_TOK_END_TAG:
      if (parser->m_tagLevel == startTagLevel)
        return XML_ERROR_ASYNC_ENTITY;
      else {
        int len;
        const char *rawName;
        TAG *tag = parser->m_tagStack;
        rawName = s + enc->minBytesPerChar * 2;
        len = XmlNameLength(enc, rawName);
        if (len != tag->rawNameLength
            || memcmp(tag->rawName, rawName, len) != 0) {
          const XML_Char *localPart;
          const XML_Char *prefix;
          XML_Char *uri;
          localPart = tag->name.localPart;
          if (parser->m_ns && localPart) {
            uri = (XML_Char *)tag->name.str + tag->name.uriLen;
            while (*localPart)
              *uri++ = *localPart++;
            prefix = tag->name.prefix;
            if (parser->m_ns_triplets && prefix) {
              *uri++ = parser->m_namespaceSeparator;
              while (*prefix)
                *uri++ = *prefix++;
            }
            *uri = XML_T('\0');
          }
          parser->m_mismatch = tag->name.str;
          *eventPP = rawName;
          return XML_ERROR_TAG_MISMATCH;
        }
        parser->m_tagStack = tag->parent;
        tag->parent = parser->m_freeTagList;
        parser->m_freeTagList = tag;
        --parser->m_tagLevel;
        if (parser->m_endElementHandler) {
          const XML_Char *localPart;
          const XML_Char *prefix;
          XML_Char *uri;
          localPart = tag->name.localPart;
          if (parser->m_ns && localPart) {
            uri = (XML_Char *)tag->name.str + tag->name.uriLen;
            while (*localPart)
              *uri++ = *localPart++;
            prefix = tag->name.prefix;
            if (parser->m_ns_triplets && prefix) {
              *uri++ = parser->m_namespaceSeparator;
              while (*prefix)
                *uri++ = *prefix++;
            }
            *uri = XML_T('\0');
          }
          beforeHandler(parser);
          parser->m_endElementHandler(parser->m_handlerArg, tag->name.str);
          afterHandler(parser);
        } else if (parser->m_defaultHandler)
          reportDefault(parser, enc, s, next);
        while (tag->bindings) {
          BINDING *b = tag->bindings;
          if (parser->m_endNamespaceDeclHandler) {
            beforeHandler(parser);
            parser->m_endNamespaceDeclHandler(parser->m_handlerArg,
                                              b->prefix->name);
            afterHandler(parser);
          }
          tag->bindings = tag->bindings->nextTagBinding;
          b->nextTagBinding = parser->m_freeBindingList;
          parser->m_freeBindingList = b;
          b->prefix->binding = b->prevPrefixBinding;
        }
        if ((parser->m_tagLevel == 0)
            && (parser->m_parsingStatus.parsing != XML_FINISHED)) {
          if (parser->m_parsingStatus.parsing == XML_SUSPENDED
              || (parser->m_parsingStatus.parsing == XML_PARSING
                  && parser->m_reenter))
            parser->m_processor = epilogProcessor;
          else
            return epilogProcessor(parser, next, end, nextPtr);
        }
      }
      break;
    case XML_TOK_CHAR_REF: {
      int n = XmlCharRefNumber(enc, s);
      if (n < 0)
        return XML_ERROR_BAD_CHAR_REF;
      if (parser->m_characterDataHandler) {
        XML_Char buf[XML_ENCODE_MAX];
        beforeHandler(parser);
        parser->m_characterDataHandler(parser->m_handlerArg, buf,
                                       XmlEncode(n, (ICHAR *)buf));
        afterHandler(parser);
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
    } break;
    case XML_TOK_XML_DECL:
      return XML_ERROR_MISPLACED_XML_PI;
    case XML_TOK_DATA_NEWLINE:
      if (parser->m_characterDataHandler) {
        XML_Char c = 0xA;
        beforeHandler(parser);
        parser->m_characterDataHandler(parser->m_handlerArg, &c, 1);
        afterHandler(parser);
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
      break;
    case XML_TOK_CDATA_SECT_OPEN: {
      enum XML_Error result;
      if (parser->m_startCdataSectionHandler) {
        beforeHandler(parser);
        parser->m_startCdataSectionHandler(parser->m_handlerArg);
        afterHandler(parser);
      } else if ((0) && parser->m_characterDataHandler) {
        beforeHandler(parser);
        parser->m_characterDataHandler(parser->m_handlerArg, parser->m_dataBuf,
                                       0);
        afterHandler(parser);
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
      result
          = doCdataSection(parser, enc, &next, end, nextPtr, haveMore, account);
      if (result != XML_ERROR_NONE)
        return result;
      else if (! next) {
        parser->m_processor = cdataSectionProcessor;
        return result;
      }
    } break;
    case XML_TOK_TRAILING_RSQB:
      if (haveMore) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      if (parser->m_characterDataHandler) {
        if (MUST_CONVERT(enc, s)) {
          ICHAR *dataPtr = (ICHAR *)parser->m_dataBuf;
          XmlConvert(enc, &s, end, &dataPtr, (ICHAR *)parser->m_dataBufEnd);
          beforeHandler(parser);
          parser->m_characterDataHandler(
              parser->m_handlerArg, parser->m_dataBuf,
              (int)(dataPtr - (ICHAR *)parser->m_dataBuf));
          afterHandler(parser);
        } else {
          beforeHandler(parser);
          parser->m_characterDataHandler(
              parser->m_handlerArg, (const XML_Char *)s,
              (int)((const XML_Char *)end - (const XML_Char *)s));
          afterHandler(parser);
        }
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, end);
      if (startTagLevel == 0) {
        *eventPP = end;
        return XML_ERROR_NO_ELEMENTS;
      }
      if (parser->m_tagLevel != startTagLevel) {
        *eventPP = end;
        return XML_ERROR_ASYNC_ENTITY;
      }
      *nextPtr = end;
      return XML_ERROR_NONE;
    case XML_TOK_DATA_CHARS: {
      XML_CharacterDataHandler charDataHandler = parser->m_characterDataHandler;
      if (charDataHandler) {
        if (MUST_CONVERT(enc, s)) {
          for (;;) {
            ICHAR *dataPtr = (ICHAR *)parser->m_dataBuf;
            const enum XML_Convert_Result convert_res = XmlConvert(
                enc, &s, next, &dataPtr, (ICHAR *)parser->m_dataBufEnd);
            *eventEndPP = s;
            beforeHandler(parser);
            charDataHandler(parser->m_handlerArg, parser->m_dataBuf,
                            (int)(dataPtr - (ICHAR *)parser->m_dataBuf));
            afterHandler(parser);
            if ((convert_res == XML_CONVERT_COMPLETED)
                || (convert_res == XML_CONVERT_INPUT_INCOMPLETE))
              break;
            *eventPP = s;
          }
        } else {
          beforeHandler(parser);
          charDataHandler(parser->m_handlerArg, (const XML_Char *)s,
                          (int)((const XML_Char *)next - (const XML_Char *)s));
          afterHandler(parser);
        }
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
    } break;
    case XML_TOK_PI:
      if (! reportProcessingInstruction(parser, enc, s, next))
        return XML_ERROR_NO_MEMORY;
      break;
    case XML_TOK_COMMENT:
      if (! reportComment(parser, enc, s, next))
        return XML_ERROR_NO_MEMORY;
      break;
    default:
      /* All of the tokens produced by XmlContentTok() have their own
       * explicit cases, so this default is not strictly necessary.
       * However it is a useful safety net, so we retain the code and
       * simply exclude it from the coverage tests.
       *
       * LCOV_EXCL_START
       */
      if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
      break;
      /* LCOV_EXCL_STOP */
    }
    switch (parser->m_parsingStatus.parsing) {
    case XML_SUSPENDED:
      *eventPP = next;
      *nextPtr = next;
      return XML_ERROR_NONE;
    case XML_FINISHED:
      *eventPP = next;
      return XML_ERROR_ABORTED;
    case XML_PARSING:
      if (parser->m_reenter) {
        *nextPtr = next;
        return XML_ERROR_NONE;
      }
      EXPAT_FALLTHROUGH;
    default:;
      *eventPP = s = next;
    }
  }
}

static void
freeBindings(XML_Parser parser, BINDING *bindings) {
  while (bindings) {
    BINDING *b = bindings;

    if (parser->m_endNamespaceDeclHandler) {
      beforeHandler(parser);
      parser->m_endNamespaceDeclHandler(parser->m_handlerArg, b->prefix->name);
      afterHandler(parser);
    }

    bindings = bindings->nextTagBinding;
    b->nextTagBinding = parser->m_freeBindingList;
    parser->m_freeBindingList = b;
    b->prefix->binding = b->prevPrefixBinding;
  }
}

static enum XML_Error
storeAtts(XML_Parser parser, const ENCODING *enc, const char *attStr,
          TAG_NAME *tagNamePtr, BINDING **bindingsPtr,
          enum XML_Account account) {
  DTD *const dtd = parser->m_dtd; 
  int attIndex = 0;
  XML_Char *uri;
  int nPrefixes = 0;
  int nXMLNSDeclarations = 0;
  BINDING *binding;
  const XML_Char *localPart;

  ELEMENT_TYPE *elementType
      = (ELEMENT_TYPE *)lookup(parser, &dtd->elementTypes, tagNamePtr->str, 0);
  if (! elementType) {
    const XML_Char *name = poolCopyString(&dtd->pool, tagNamePtr->str);
    if (! name)
      return XML_ERROR_NO_MEMORY;
    elementType = (ELEMENT_TYPE *)lookup(parser, &dtd->elementTypes, name,
                                         sizeof(ELEMENT_TYPE));
    if (! elementType)
      return XML_ERROR_NO_MEMORY;
    if (! elementType->defaultAttsNames.parser)
      hashTableInit(&(elementType->defaultAttsNames), parser);
    if (parser->m_ns && ! setElementTypePrefix(parser, elementType))
      return XML_ERROR_NO_MEMORY;
  }
  const size_t nDefaultAtts = elementType->nDefaultAtts;

  if (parser->m_attsSize > (size_t)INT_MAX)
    return XML_ERROR_NO_MEMORY;

  size_t n = (size_t)XmlGetAttributes(enc, attStr, (int)parser->m_attsSize,
                                      parser->m_atts);

  if (n > SIZE_MAX - nDefaultAtts) {
    return XML_ERROR_NO_MEMORY;
  }

  if (n + nDefaultAtts > parser->m_attsSize) {
    size_t oldAttsSize = parser->m_attsSize;

    if ((nDefaultAtts > SIZE_MAX - INIT_ATTS_SIZE)
        || (n > SIZE_MAX - (nDefaultAtts + INIT_ATTS_SIZE))) {
      return XML_ERROR_NO_MEMORY;
    }

    parser->m_attsSize = n + nDefaultAtts + INIT_ATTS_SIZE;

    if (parser->m_attsSize > SIZE_MAX / sizeof(ATTRIBUTE)) {
      parser->m_attsSize = oldAttsSize;
      return XML_ERROR_NO_MEMORY;
    }

    ATTRIBUTE *const temp = REALLOC(parser, parser->m_atts,
                                    parser->m_attsSize * sizeof(ATTRIBUTE));
    if (temp == NULL) {
      parser->m_attsSize = oldAttsSize;
      return XML_ERROR_NO_MEMORY;
    }
    parser->m_atts = temp;
#if defined(XML_ATTR_INFO)
    if (parser->m_attsSize > SIZE_MAX / sizeof(XML_AttrInfo)) {
      parser->m_attsSize = oldAttsSize;
      return XML_ERROR_NO_MEMORY;
    }

    XML_AttrInfo *const temp2 = REALLOC(
        parser, parser->m_attInfo, parser->m_attsSize * sizeof(XML_AttrInfo));
    if (temp2 == NULL) {
      parser->m_attsSize = oldAttsSize;
      return XML_ERROR_NO_MEMORY;
    }
    parser->m_attInfo = temp2;
#endif
    if (n > oldAttsSize) {
      if (n > (size_t)INT_MAX)
        return XML_ERROR_NO_MEMORY;
      XmlGetAttributes(enc, attStr, (int)n, parser->m_atts);
    }
  }

  const XML_Char **const appAtts = (const XML_Char **)parser->m_atts;
  for (size_t i = 0; i < n; i++) {
    ATTRIBUTE *currAtt = &parser->m_atts[i];
#if defined(XML_ATTR_INFO)
    XML_AttrInfo *currAttInfo = &parser->m_attInfo[i];
#endif
    ATTRIBUTE_ID *attId
        = getAttributeId(parser, enc, currAtt->name,
                         currAtt->name + XmlNameLength(enc, currAtt->name));
    if (! attId)
      return XML_ERROR_NO_MEMORY;
#if defined(XML_ATTR_INFO)
    currAttInfo->nameStart
        = parser->m_parseEndByteIndex - (parser->m_parseEndPtr - currAtt->name);
    currAttInfo->nameEnd
        = currAttInfo->nameStart + XmlNameLength(enc, currAtt->name);
    currAttInfo->valueStart = parser->m_parseEndByteIndex
                              - (parser->m_parseEndPtr - currAtt->valuePtr);
    currAttInfo->valueEnd = parser->m_parseEndByteIndex
                            - (parser->m_parseEndPtr - currAtt->valueEnd);
#endif
    if ((attId->name)[-1]) {
      if (enc == parser->m_encoding)
        parser->m_eventPtr = parser->m_atts[i].name;
      return XML_ERROR_DUPLICATE_ATTRIBUTE;
    }
    (attId->name)[-1] = 1;
    appAtts[attIndex++] = attId->name;
    if (! parser->m_atts[i].normalized) {
      XML_Bool isCdata = XML_TRUE;

      if (attId->maybeTokenized) {
        for (size_t j = 0; j < nDefaultAtts; j++) {
          if (attId == elementType->defaultAtts[j].id) {
            isCdata = elementType->defaultAtts[j].isCdata;
            break;
          }
        }
      }

      const enum XML_Error result = storeAttributeValue(
          parser, enc, isCdata, parser->m_atts[i].valuePtr,
          parser->m_atts[i].valueEnd, &parser->m_tempPool, account);
      if (result)
        return result;
      appAtts[attIndex] = poolStart(&parser->m_tempPool);
      poolFinish(&parser->m_tempPool);
    } else {
      appAtts[attIndex] = poolStoreString(&parser->m_tempPool, enc,
                                          parser->m_atts[i].valuePtr,
                                          parser->m_atts[i].valueEnd);
      if (appAtts[attIndex] == 0)
        return XML_ERROR_NO_MEMORY;
      poolFinish(&parser->m_tempPool);
    }
    if (attId->prefix) {
      if (attId->xmlns) {
        enum XML_Error result = addBinding(parser, attId->prefix, attId,
                                           appAtts[attIndex], bindingsPtr);
        if (result)
          return result;
        attIndex++;
        nXMLNSDeclarations++;
        (attId->name)[-1] = 3;
      } else {
        attIndex++;
        nPrefixes++;
        (attId->name)[-1] = 2;
      }
    } else
      attIndex++;
  }

  parser->m_nSpecifiedAtts = attIndex;
  if (elementType->idAtt && (elementType->idAtt->name)[-1]) {
    for (int i = 0; i < attIndex; i += 2)
      if (appAtts[i] == elementType->idAtt->name) {
        parser->m_idAttIndex = i;
        break;
      }
  } else
    parser->m_idAttIndex = -1;

  for (size_t i = 0; i < nDefaultAtts; i++) {
    const DEFAULT_ATTRIBUTE *da = elementType->defaultAtts + i;
    if (! (da->id->name)[-1] && da->value) {
      if (da->id->prefix) {
        if (da->id->xmlns) {
          enum XML_Error result = addBinding(parser, da->id->prefix, da->id,
                                             da->value, bindingsPtr);
          if (result)
            return result;
          (da->id->name)[-1] = 3;
          nXMLNSDeclarations++;
          appAtts[attIndex++] = da->id->name;
          appAtts[attIndex++] = da->value;
        } else {
          (da->id->name)[-1] = 2;
          nPrefixes++;
          appAtts[attIndex++] = da->id->name;
          appAtts[attIndex++] = da->value;
        }
      } else {
        (da->id->name)[-1] = 1;
        appAtts[attIndex++] = da->id->name;
        appAtts[attIndex++] = da->value;
      }
    }
  }
  appAtts[attIndex] = 0;

  int i = 0;
  if (nPrefixes || nXMLNSDeclarations) {
    unsigned int j; 
    unsigned long version = parser->m_nsAttsVersion;

    if (parser->m_nsAttsPower >= sizeof(unsigned int) * 8 ) {
      return XML_ERROR_NO_MEMORY;
    }

    unsigned int nsAttsSize = 1u << parser->m_nsAttsPower;
    if (nPrefixes) {
    unsigned char oldNsAttsPower = parser->m_nsAttsPower;
    if ((nPrefixes << 1)
        >> parser->m_nsAttsPower) { 
      while (nPrefixes >> parser->m_nsAttsPower++)
        ;
      if (parser->m_nsAttsPower < 3)
        parser->m_nsAttsPower = 3;

      if (parser->m_nsAttsPower >= sizeof(nsAttsSize) * 8 ) {
        parser->m_nsAttsPower = oldNsAttsPower;
        return XML_ERROR_NO_MEMORY;
      }

      nsAttsSize = 1u << parser->m_nsAttsPower;

#if UINT_MAX >= SIZE_MAX
      if (nsAttsSize > SIZE_MAX / sizeof(NS_ATT)) {
        parser->m_nsAttsPower = oldNsAttsPower;
        return XML_ERROR_NO_MEMORY;
      }
#endif

      NS_ATT *const temp
          = REALLOC(parser, parser->m_nsAtts, nsAttsSize * sizeof(NS_ATT));
      if (! temp) {
        parser->m_nsAttsPower = oldNsAttsPower;
        return XML_ERROR_NO_MEMORY;
      }
      parser->m_nsAtts = temp;
      version = 0; 
    }
    if (! version) { 
      version = INIT_ATTS_VERSION;
      for (j = nsAttsSize; j != 0;)
        parser->m_nsAtts[--j].version = version;
    }
    parser->m_nsAttsVersion = --version;
    }

    for (; i < attIndex; i += 2) {
      const XML_Char *s = appAtts[i];
      if (s[-1] == 2) { 
        struct siphash sip_state;
        struct sipkey sip_key;

        copy_salt_to_sipkey(parser, &sip_key);
        sip24_init(&sip_state, &sip_key);

        ((XML_Char *)s)[-1] = 0; 
        ATTRIBUTE_ID *const id
            = (ATTRIBUTE_ID *)lookup(parser, &dtd->attributeIds, s, 0);
        if (! id || ! id->prefix) {
          return XML_ERROR_NO_MEMORY; /* LCOV_EXCL_LINE */
        }
        const BINDING *const b = id->prefix->binding;
        if (! b)
          return XML_ERROR_UNBOUND_PREFIX;

        if (! poolAppendChars(&parser->m_tempPool, b->uri, b->uriLen))
          return XML_ERROR_NO_MEMORY;

        sip24_update(&sip_state, b->uri, b->uriLen * sizeof(XML_Char));

        while (*s++ != XML_T(ASCII_COLON))
          ;

        sip24_update(&sip_state, s, keylen(s) * sizeof(XML_Char));

        {
          const size_t len = xcslen(s) +  1;
          if (! poolAppendChars(&parser->m_tempPool, s, len))
            return XML_ERROR_NO_MEMORY;
        }

        const unsigned long uriHash = (unsigned long)sip24_final(&sip_state);

        { 
          unsigned char step = 0;
          unsigned long mask = nsAttsSize - 1;
          j = uriHash & mask; 
          while (parser->m_nsAtts[j].version == version) {
            if (uriHash == parser->m_nsAtts[j].hash) {
              const XML_Char *s1 = poolStart(&parser->m_tempPool);
              const XML_Char *s2 = parser->m_nsAtts[j].uriName;
              for (; *s1 == *s2 && *s1 != 0; s1++, s2++)
                ;
              if (*s1 == 0)
                return XML_ERROR_DUPLICATE_ATTRIBUTE;
            }
            if (! step)
              step = PROBE_STEP(uriHash, mask, parser->m_nsAttsPower);
            j < step ? (j += nsAttsSize - step) : (j -= step);
          }
        }

        if (parser->m_ns_triplets) { 
          parser->m_tempPool.ptr[-1] = parser->m_namespaceSeparator;
          s = b->prefix->name;
          const size_t len = xcslen(s) +  1;
          if (! poolAppendChars(&parser->m_tempPool, s, len))
            return XML_ERROR_NO_MEMORY;
        }

        s = poolStart(&parser->m_tempPool);
        poolFinish(&parser->m_tempPool);
        appAtts[i] = s;

        parser->m_nsAtts[j].version = version;
        parser->m_nsAtts[j].hash = uriHash;
        parser->m_nsAtts[j].uriName = s;

        if (! --nPrefixes && ! nXMLNSDeclarations) {
          i += 2;
          break;
        }
      } else if (s[-1] == 3) { 
        static const XML_Char xmlnsNamespace[] = {
          ASCII_h, ASCII_t, ASCII_t, ASCII_p, ASCII_COLON, ASCII_SLASH, ASCII_SLASH,
          ASCII_w, ASCII_w, ASCII_w, ASCII_PERIOD, ASCII_w, ASCII_3, ASCII_PERIOD,
          ASCII_o, ASCII_r, ASCII_g, ASCII_SLASH, ASCII_2, ASCII_0, ASCII_0, ASCII_0,
          ASCII_SLASH, ASCII_x, ASCII_m, ASCII_l, ASCII_n, ASCII_s, ASCII_SLASH, '\0'
        };
        static const XML_Char xmlnsPrefix[] = {
          ASCII_x, ASCII_m, ASCII_l, ASCII_n, ASCII_s, '\0'
        };

        ((XML_Char *)s)[-1] = 0;  
        if (! poolAppendString(&parser->m_tempPool, xmlnsNamespace)
            || ! poolAppendChar(&parser->m_tempPool, parser->m_namespaceSeparator))
          return XML_ERROR_NO_MEMORY;
        s += sizeof(xmlnsPrefix) / sizeof(xmlnsPrefix[0]) - 1;
        if (*s == XML_T(':')) {
          ++s;
          do {  
            if (! poolAppendChar(&parser->m_tempPool, *s))
              return XML_ERROR_NO_MEMORY;
          } while (*s++);
          if (parser->m_ns_triplets) { 
            parser->m_tempPool.ptr[-1] = parser->m_namespaceSeparator;
            if (! poolAppendString(&parser->m_tempPool, xmlnsPrefix)
                || ! poolAppendChar(&parser->m_tempPool, '\0'))
              return XML_ERROR_NO_MEMORY;
          }
        }
        else {
          if (! poolAppendString(&parser->m_tempPool, xmlnsPrefix)
              || ! poolAppendChar(&parser->m_tempPool, '\0'))
            return XML_ERROR_NO_MEMORY;
        }

        s = poolStart(&parser->m_tempPool);
        poolFinish(&parser->m_tempPool);
        appAtts[i] = s;

        if (! --nXMLNSDeclarations && ! nPrefixes) {
          i += 2;
          break;
        }
      } else                     
        ((XML_Char *)s)[-1] = 0; 
    }
  }
  for (; i < attIndex; i += 2)
    ((XML_Char *)(appAtts[i]))[-1] = 0;
  for (binding = *bindingsPtr; binding; binding = binding->nextTagBinding)
    binding->attId->name[-1] = 0;

  if (! parser->m_ns)
    return XML_ERROR_NONE;

  if (elementType->prefix) {
    binding = elementType->prefix->binding;
    if (! binding)
      return XML_ERROR_UNBOUND_PREFIX;
    localPart = tagNamePtr->str;
    while (*localPart++ != XML_T(ASCII_COLON))
      ;
  } else if (dtd->defaultPrefix.binding) {
    binding = dtd->defaultPrefix.binding;
    localPart = tagNamePtr->str;
  } else
    return XML_ERROR_NONE;
  size_t prefixLen = 0;
  if (parser->m_ns_triplets && binding->prefix->name)
    prefixLen = xcslen(binding->prefix->name) +  1;
  tagNamePtr->localPart = localPart;
  tagNamePtr->uriLen = binding->uriLen;
  tagNamePtr->prefix = binding->prefix->name;
  tagNamePtr->prefixLen = prefixLen;

  const size_t localPartLen = xcslen(localPart) +  1;

  if (binding->uriLen > SIZE_MAX - prefixLen
      || localPartLen > SIZE_MAX - (binding->uriLen + prefixLen)) {
    return XML_ERROR_NO_MEMORY;
  }

  const size_t totalLen = localPartLen + binding->uriLen + prefixLen;
  if (totalLen > binding->uriAlloc) {
    if (totalLen > SIZE_MAX - EXPAND_SPARE
        || totalLen + EXPAND_SPARE > SIZE_MAX / sizeof(XML_Char)) {
      return XML_ERROR_NO_MEMORY;
    }

    uri = MALLOC(parser, (totalLen + EXPAND_SPARE) * sizeof(XML_Char));
    if (! uri)
      return XML_ERROR_NO_MEMORY;
    binding->uriAlloc = totalLen + EXPAND_SPARE;
    memcpy(uri, binding->uri, binding->uriLen * sizeof(XML_Char));
    for (TAG *p = parser->m_tagStack; p; p = p->parent)
      if (p->name.str == binding->uri)
        p->name.str = uri;
    FREE(parser, binding->uri);
    binding->uri = uri;
  }
  uri = binding->uri + binding->uriLen;
  if (localPartLen > SIZE_MAX / sizeof(XML_Char)) {
    return XML_ERROR_NO_MEMORY;
  }
  memcpy(uri, localPart, localPartLen * sizeof(XML_Char));
  if (prefixLen) {
    uri += localPartLen - 1;
    *uri = parser->m_namespaceSeparator; 
    memcpy(uri + 1, binding->prefix->name, prefixLen * sizeof(XML_Char));
  }
  tagNamePtr->str = binding->uri;
  return XML_ERROR_NONE;
}

static XML_Bool
is_rfc3986_uri_char(XML_Char candidate) {

  switch (candidate) {
  case 'A':
  case 'B':
  case 'C':
  case 'D':
  case 'E':
  case 'F':
  case 'G':
  case 'H':
  case 'I':
  case 'J':
  case 'K':
  case 'L':
  case 'M':
  case 'N':
  case 'O':
  case 'P':
  case 'Q':
  case 'R':
  case 'S':
  case 'T':
  case 'U':
  case 'V':
  case 'W':
  case 'X':
  case 'Y':
  case 'Z':

  case 'a':
  case 'b':
  case 'c':
  case 'd':
  case 'e':
  case 'f':
  case 'g':
  case 'h':
  case 'i':
  case 'j':
  case 'k':
  case 'l':
  case 'm':
  case 'n':
  case 'o':
  case 'p':
  case 'q':
  case 'r':
  case 's':
  case 't':
  case 'u':
  case 'v':
  case 'w':
  case 'x':
  case 'y':
  case 'z':

  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':

  case '%':

  case '-':
  case '.':
  case '_':
  case '~':

  case ':':
  case '/':
  case '?':
  case '#':
  case '[':
  case ']':
  case '@':

  case '!':
  case '$':
  case '&':
  case '\'':
  case '(':
  case ')':
  case '*':
  case '+':
  case ',':
  case ';':
  case '=':
    return XML_TRUE;

  default:
    return XML_FALSE;
  }
}

static enum XML_Error
addBinding(XML_Parser parser, PREFIX *prefix, const ATTRIBUTE_ID *attId,
           const XML_Char *uri, BINDING **bindingsPtr) {
  static const XML_Char xmlNamespace[]
      = {ASCII_h,      ASCII_t,     ASCII_t,     ASCII_p,      ASCII_COLON,
         ASCII_SLASH,  ASCII_SLASH, ASCII_w,     ASCII_w,      ASCII_w,
         ASCII_PERIOD, ASCII_w,     ASCII_3,     ASCII_PERIOD, ASCII_o,
         ASCII_r,      ASCII_g,     ASCII_SLASH, ASCII_X,      ASCII_M,
         ASCII_L,      ASCII_SLASH, ASCII_1,     ASCII_9,      ASCII_9,
         ASCII_8,      ASCII_SLASH, ASCII_n,     ASCII_a,      ASCII_m,
         ASCII_e,      ASCII_s,     ASCII_p,     ASCII_a,      ASCII_c,
         ASCII_e,      '\0'};
  static const size_t xmlLen = sizeof(xmlNamespace) / sizeof(XML_Char) - 1;
  static const XML_Char xmlnsNamespace[]
      = {ASCII_h,     ASCII_t,      ASCII_t, ASCII_p, ASCII_COLON,  ASCII_SLASH,
         ASCII_SLASH, ASCII_w,      ASCII_w, ASCII_w, ASCII_PERIOD, ASCII_w,
         ASCII_3,     ASCII_PERIOD, ASCII_o, ASCII_r, ASCII_g,      ASCII_SLASH,
         ASCII_2,     ASCII_0,      ASCII_0, ASCII_0, ASCII_SLASH,  ASCII_x,
         ASCII_m,     ASCII_l,      ASCII_n, ASCII_s, ASCII_SLASH,  '\0'};
  static const size_t xmlnsLen = sizeof(xmlnsNamespace) / sizeof(XML_Char) - 1;

  XML_Bool mustBeXML = XML_FALSE;
  XML_Bool isXML = XML_TRUE;
  XML_Bool isXMLNS = XML_TRUE;

  BINDING *b;
  size_t len;

  if (*uri == XML_T('\0') && prefix->name)
    return XML_ERROR_UNDECLARING_PREFIX;

  if (prefix->name && prefix->name[0] == XML_T(ASCII_x)
      && prefix->name[1] == XML_T(ASCII_m)
      && prefix->name[2] == XML_T(ASCII_l)) {
    if (prefix->name[3] == XML_T(ASCII_n) && prefix->name[4] == XML_T(ASCII_s)
        && prefix->name[5] == XML_T('\0'))
      return XML_ERROR_RESERVED_PREFIX_XMLNS;

    if (prefix->name[3] == XML_T('\0'))
      mustBeXML = XML_TRUE;
  }

  for (len = 0; uri[len]; len++) {
    if (len == SIZE_MAX) {
      return XML_ERROR_NO_MEMORY;
    }
    if (isXML && (len > xmlLen || uri[len] != xmlNamespace[len]))
      isXML = XML_FALSE;

    if (! mustBeXML && isXMLNS
        && (len > xmlnsLen || uri[len] != xmlnsNamespace[len]))
      isXMLNS = XML_FALSE;

    if (parser->m_ns && (uri[len] == parser->m_namespaceSeparator)
        && ! is_rfc3986_uri_char(uri[len])) {
      return XML_ERROR_SYNTAX;
    }
  }
  isXML = isXML && len == xmlLen;
  isXMLNS = isXMLNS && len == xmlnsLen;

  if (mustBeXML != isXML)
    return mustBeXML ? XML_ERROR_RESERVED_PREFIX_XML
                     : XML_ERROR_RESERVED_NAMESPACE_URI;

  if (isXMLNS)
    return XML_ERROR_RESERVED_NAMESPACE_URI;

  if (parser->m_namespaceSeparator) {
    if (len == SIZE_MAX) {
      return XML_ERROR_NO_MEMORY;
    }
    len++;
  }
  if (parser->m_freeBindingList) {
    b = parser->m_freeBindingList;
    if (len > b->uriAlloc) {
      if (len > SIZE_MAX - EXPAND_SPARE
          || len + EXPAND_SPARE > SIZE_MAX / sizeof(XML_Char)) {
        return XML_ERROR_NO_MEMORY;
      }

      XML_Char *temp
          = REALLOC(parser, b->uri, sizeof(XML_Char) * (len + EXPAND_SPARE));
      if (temp == NULL)
        return XML_ERROR_NO_MEMORY;
      b->uri = temp;
      b->uriAlloc = len + EXPAND_SPARE;
    }
    parser->m_freeBindingList = b->nextTagBinding;
  } else {
    b = MALLOC(parser, sizeof(BINDING));
    if (! b)
      return XML_ERROR_NO_MEMORY;

    if (len > SIZE_MAX - EXPAND_SPARE
        || len + EXPAND_SPARE > SIZE_MAX / sizeof(XML_Char)) {
      return XML_ERROR_NO_MEMORY;
    }

    b->uri = MALLOC(parser, sizeof(XML_Char) * (len + EXPAND_SPARE));
    if (! b->uri) {
      FREE(parser, b);
      return XML_ERROR_NO_MEMORY;
    }
    b->uriAlloc = len + EXPAND_SPARE;
  }
  b->uriLen = len;
  memcpy(b->uri, uri, len * sizeof(XML_Char));
  if (parser->m_namespaceSeparator)
    b->uri[len - 1] = parser->m_namespaceSeparator;
  b->prefix = prefix;
  b->attId = attId;
  b->prevPrefixBinding = prefix->binding;
  if (*uri == XML_T('\0') && prefix == &parser->m_dtd->defaultPrefix)
    prefix->binding = NULL;
  else
    prefix->binding = b;
  b->nextTagBinding = *bindingsPtr;
  *bindingsPtr = b;
  if (attId && parser->m_startNamespaceDeclHandler) {
    beforeHandler(parser);
    parser->m_startNamespaceDeclHandler(parser->m_handlerArg, prefix->name,
                                        prefix->binding ? uri : 0);
    afterHandler(parser);
  }
  return XML_ERROR_NONE;
}

static enum XML_Error PTRCALL
cdataSectionProcessor(XML_Parser parser, const char *start, const char *end,
                      const char **endPtr) {
  enum XML_Error result = doCdataSection(
      parser, parser->m_encoding, &start, end, endPtr,
      (XML_Bool)! parser->m_parsingStatus.finalBuffer, XML_ACCOUNT_DIRECT);
  if (result != XML_ERROR_NONE)
    return result;
  if (start) {
    if (parser->m_parentParser) { 
      parser->m_processor = externalEntityContentProcessor;
      return externalEntityContentProcessor(parser, start, end, endPtr);
    } else {
      parser->m_processor = contentProcessor;
      return contentProcessor(parser, start, end, endPtr);
    }
  }
  return result;
}

static enum XML_Error
doCdataSection(XML_Parser parser, const ENCODING *enc, const char **startPtr,
               const char *end, const char **nextPtr, XML_Bool haveMore,
               enum XML_Account account) {
  const char *s = *startPtr;
  const char **eventPP;
  const char **eventEndPP;
  if (enc == parser->m_encoding) {
    eventPP = &parser->m_eventPtr;
    *eventPP = s;
    eventEndPP = &parser->m_eventEndPtr;
  } else {
    eventPP = &(parser->m_openInternalEntities->internalEventPtr);
    eventEndPP = &(parser->m_openInternalEntities->internalEventEndPtr);
  }
  *eventPP = s;
  *startPtr = NULL;

  for (;;) {
    const char *next = s; 
    int tok = XmlCdataSectionTok(enc, s, end, &next);
#if XML_GE == 1
    if (! accountingDiffTolerated(parser, tok, s, next, __LINE__, account)) {
      accountingOnAbort(parser);
      return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
    }
#else
    UNUSED_P(account);
#endif
    *eventEndPP = next;
    switch (tok) {
    case XML_TOK_CDATA_SECT_CLOSE:
      if (parser->m_endCdataSectionHandler) {
        beforeHandler(parser);
        parser->m_endCdataSectionHandler(parser->m_handlerArg);
        afterHandler(parser);
      }
      else if ((0) && parser->m_characterDataHandler) {
        beforeHandler(parser);
        parser->m_characterDataHandler(parser->m_handlerArg, parser->m_dataBuf,
                                       0);
        afterHandler(parser);
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
      *startPtr = next;
      *nextPtr = next;
      if (parser->m_parsingStatus.parsing == XML_FINISHED)
        return XML_ERROR_ABORTED;
      else
        return XML_ERROR_NONE;
    case XML_TOK_DATA_NEWLINE:
      if (parser->m_characterDataHandler) {
        XML_Char c = 0xA;
        beforeHandler(parser);
        parser->m_characterDataHandler(parser->m_handlerArg, &c, 1);
        afterHandler(parser);
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
      break;
    case XML_TOK_DATA_CHARS: {
      XML_CharacterDataHandler charDataHandler = parser->m_characterDataHandler;
      if (charDataHandler) {
        if (MUST_CONVERT(enc, s)) {
          for (;;) {
            ICHAR *dataPtr = (ICHAR *)parser->m_dataBuf;
            const enum XML_Convert_Result convert_res = XmlConvert(
                enc, &s, next, &dataPtr, (ICHAR *)parser->m_dataBufEnd);
            *eventEndPP = next;
            beforeHandler(parser);
            charDataHandler(parser->m_handlerArg, parser->m_dataBuf,
                            (int)(dataPtr - (ICHAR *)parser->m_dataBuf));
            afterHandler(parser);
            if ((convert_res == XML_CONVERT_COMPLETED)
                || (convert_res == XML_CONVERT_INPUT_INCOMPLETE))
              break;
            *eventPP = s;
          }
        } else {
          beforeHandler(parser);
          charDataHandler(parser->m_handlerArg, (const XML_Char *)s,
                          (int)((const XML_Char *)next - (const XML_Char *)s));
          afterHandler(parser);
        }
      } else if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
    } break;
    case XML_TOK_INVALID:
      *eventPP = next;
      return XML_ERROR_INVALID_TOKEN;
    case XML_TOK_PARTIAL_CHAR:
      if (haveMore) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      return XML_ERROR_PARTIAL_CHAR;
    case XML_TOK_PARTIAL:
    case XML_TOK_NONE:
      if (haveMore) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      return XML_ERROR_UNCLOSED_CDATA_SECTION;
    default:
      /* Every token returned by XmlCdataSectionTok() has its own
       * explicit case, so this default case will never be executed.
       * We retain it as a safety net and exclude it from the coverage
       * statistics.
       *
       * LCOV_EXCL_START
       */
      *eventPP = next;
      return XML_ERROR_UNEXPECTED_STATE;
      /* LCOV_EXCL_STOP */
    }

    switch (parser->m_parsingStatus.parsing) {
    case XML_SUSPENDED:
      *eventPP = next;
      *nextPtr = next;
      return XML_ERROR_NONE;
    case XML_FINISHED:
      *eventPP = next;
      return XML_ERROR_ABORTED;
    case XML_PARSING:
      if (parser->m_reenter) {
        return XML_ERROR_UNEXPECTED_STATE; // LCOV_EXCL_LINE
      }
      EXPAT_FALLTHROUGH;
    default:;
      *eventPP = s = next;
    }
  }
}

#if defined(XML_DTD)

static enum XML_Error PTRCALL
ignoreSectionProcessor(XML_Parser parser, const char *start, const char *end,
                       const char **endPtr) {
  enum XML_Error result
      = doIgnoreSection(parser, parser->m_encoding, &start, end, endPtr,
                        (XML_Bool)! parser->m_parsingStatus.finalBuffer);
  if (result != XML_ERROR_NONE)
    return result;
  if (start) {
    parser->m_processor = prologProcessor;
    return prologProcessor(parser, start, end, endPtr);
  }
  return result;
}

static enum XML_Error
doIgnoreSection(XML_Parser parser, const ENCODING *enc, const char **startPtr,
                const char *end, const char **nextPtr, XML_Bool haveMore) {
  const char *next = *startPtr; 
  int tok;
  const char *s = *startPtr;
  const char **eventPP;
  const char **eventEndPP;
  if (enc == parser->m_encoding) {
    eventPP = &parser->m_eventPtr;
    *eventPP = s;
    eventEndPP = &parser->m_eventEndPtr;
  } else {
    /* It's not entirely clear, but it seems the following two lines
     * of code cannot be executed.  The only occasions on which 'enc'
     * is not 'encoding' are when this function is called
     * from the internal entity processing, and IGNORE sections are an
     * error in internal entities.
     *
     * Since it really isn't clear that this is true, we keep the code
     * and just remove it from our coverage tests.
     *
     * LCOV_EXCL_START
     */
    eventPP = &(parser->m_openInternalEntities->internalEventPtr);
    eventEndPP = &(parser->m_openInternalEntities->internalEventEndPtr);
    /* LCOV_EXCL_STOP */
  }
  *eventPP = s;
  *startPtr = NULL;
  tok = XmlIgnoreSectionTok(enc, s, end, &next);
#if XML_GE == 1
  if (! accountingDiffTolerated(parser, tok, s, next, __LINE__,
                                XML_ACCOUNT_DIRECT)) {
    accountingOnAbort(parser);
    return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
  }
#endif
  *eventEndPP = next;
  switch (tok) {
  case XML_TOK_IGNORE_SECT:
    if (parser->m_defaultHandler)
      reportDefault(parser, enc, s, next);
    *startPtr = next;
    *nextPtr = next;
    if (parser->m_parsingStatus.parsing == XML_FINISHED)
      return XML_ERROR_ABORTED;
    else
      return XML_ERROR_NONE;
  case XML_TOK_INVALID:
    *eventPP = next;
    return XML_ERROR_INVALID_TOKEN;
  case XML_TOK_PARTIAL_CHAR:
    if (haveMore) {
      *nextPtr = s;
      return XML_ERROR_NONE;
    }
    return XML_ERROR_PARTIAL_CHAR;
  case XML_TOK_PARTIAL:
  case XML_TOK_NONE:
    if (haveMore) {
      *nextPtr = s;
      return XML_ERROR_NONE;
    }
    return XML_ERROR_SYNTAX; 
  default:
    /* All of the tokens that XmlIgnoreSectionTok() returns have
     * explicit cases to handle them, so this default case is never
     * executed.  We keep it as a safety net anyway, and remove it
     * from our test coverage statistics.
     *
     * LCOV_EXCL_START
     */
    *eventPP = next;
    return XML_ERROR_UNEXPECTED_STATE;
    /* LCOV_EXCL_STOP */
  }
}

#endif

static enum XML_Error
initializeEncoding(XML_Parser parser) {
  const char *s;
#if defined(XML_UNICODE)
  char encodingBuf[128];
  if (! parser->m_protocolEncodingName)
    s = NULL;
  else {
    int i;
    for (i = 0; parser->m_protocolEncodingName[i]; i++) {
      if (i == sizeof(encodingBuf) - 1
          || (parser->m_protocolEncodingName[i] & ~0x7f) != 0) {
        encodingBuf[0] = '\0';
        break;
      }
      encodingBuf[i] = (char)parser->m_protocolEncodingName[i];
    }
    encodingBuf[i] = '\0';
    s = encodingBuf;
  }
#else
  s = parser->m_protocolEncodingName;
#endif
  if ((parser->m_ns ? XmlInitEncodingNS : XmlInitEncoding)(
          &parser->m_initEncoding, &parser->m_encoding, s))
    return XML_ERROR_NONE;
  return handleUnknownEncoding(parser, parser->m_protocolEncodingName);
}

static enum XML_Error
processXmlDecl(XML_Parser parser, int isGeneralTextEntity, const char *s,
               const char *next) {
  const char *encodingName = NULL;
  const XML_Char *storedEncName = NULL;
  const ENCODING *newEncoding = NULL;
  const char *version = NULL;
  const char *versionend = NULL;
  const XML_Char *storedversion = NULL;
  int standalone = -1;

#if XML_GE == 1
  if (! accountingDiffTolerated(parser, XML_TOK_XML_DECL, s, next, __LINE__,
                                XML_ACCOUNT_DIRECT)) {
    accountingOnAbort(parser);
    return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
  }
#endif

  if (! (parser->m_ns ? XmlParseXmlDeclNS : XmlParseXmlDecl)(
          isGeneralTextEntity, parser->m_encoding, s, next, &parser->m_eventPtr,
          &version, &versionend, &encodingName, &newEncoding, &standalone)) {
    if (isGeneralTextEntity)
      return XML_ERROR_TEXT_DECL;
    else
      return XML_ERROR_XML_DECL;
  }
  if (! isGeneralTextEntity && standalone == 1) {
    parser->m_dtd->standalone = XML_TRUE;
#if defined(XML_DTD)
    if (parser->m_paramEntityParsing
        == XML_PARAM_ENTITY_PARSING_UNLESS_STANDALONE)
      parser->m_paramEntityParsing = XML_PARAM_ENTITY_PARSING_NEVER;
#endif
  }
  if (parser->m_xmlDeclHandler) {
    if (encodingName != NULL) {
      storedEncName = poolStoreString(
          &parser->m_temp2Pool, parser->m_encoding, encodingName,
          encodingName + XmlNameLength(parser->m_encoding, encodingName));
      if (! storedEncName)
        return XML_ERROR_NO_MEMORY;
      poolFinish(&parser->m_temp2Pool);
    }
    if (version) {
      storedversion
          = poolStoreString(&parser->m_temp2Pool, parser->m_encoding, version,
                            versionend - parser->m_encoding->minBytesPerChar);
      if (! storedversion)
        return XML_ERROR_NO_MEMORY;
    }
    beforeHandler(parser);
    parser->m_xmlDeclHandler(parser->m_handlerArg, storedversion, storedEncName,
                             standalone);
    afterHandler(parser);
  } else if (parser->m_defaultHandler)
    reportDefault(parser, parser->m_encoding, s, next);
  if (parser->m_protocolEncodingName == NULL) {
    if (newEncoding) {
      if (newEncoding->minBytesPerChar != parser->m_encoding->minBytesPerChar
          || (newEncoding->minBytesPerChar == 2
              && newEncoding != parser->m_encoding)) {
        parser->m_eventPtr = encodingName;
        return XML_ERROR_INCORRECT_ENCODING;
      }
      parser->m_encoding = newEncoding;
    } else if (encodingName) {
      enum XML_Error result;
      if (! storedEncName) {
        storedEncName = poolStoreString(
            &parser->m_temp2Pool, parser->m_encoding, encodingName,
            encodingName + XmlNameLength(parser->m_encoding, encodingName));
        if (! storedEncName)
          return XML_ERROR_NO_MEMORY;
      }
      result = handleUnknownEncoding(parser, storedEncName);
      poolClear(&parser->m_temp2Pool);
      if (result == XML_ERROR_UNKNOWN_ENCODING)
        parser->m_eventPtr = encodingName;
      return result;
    }
  }

  if (storedEncName || storedversion)
    poolClear(&parser->m_temp2Pool);

  return XML_ERROR_NONE;
}

static enum XML_Error
handleUnknownEncoding(XML_Parser parser, const XML_Char *encodingName) {
  if (parser->m_unknownEncodingHandler) {
    XML_Encoding info;
    int i;
    for (i = 0; i < 256; i++)
      info.map[i] = -1;
    info.convert = NULL;
    info.data = NULL;
    info.release = NULL;
    beforeHandler(parser);
    const int status = parser->m_unknownEncodingHandler(
        parser->m_unknownEncodingHandlerData, encodingName, &info);
    afterHandler(parser);
    if (status) {
      ENCODING *enc;
      parser->m_unknownEncodingMem = MALLOC(parser, XmlSizeOfUnknownEncoding());
      if (! parser->m_unknownEncodingMem) {
        if (info.release)
          info.release(info.data);
        return XML_ERROR_NO_MEMORY;
      }
      enc = (parser->m_ns ? XmlInitUnknownEncodingNS : XmlInitUnknownEncoding)(
          parser->m_unknownEncodingMem, info.map, info.convert, info.data);
      if (enc) {
        parser->m_unknownEncodingData = info.data;
        parser->m_unknownEncodingRelease = info.release;
        parser->m_encoding = enc;
        return XML_ERROR_NONE;
      }
    }
    if (info.release != NULL)
      info.release(info.data);
  }
  return XML_ERROR_UNKNOWN_ENCODING;
}

static enum XML_Error PTRCALL
prologInitProcessor(XML_Parser parser, const char *s, const char *end,
                    const char **nextPtr) {
  enum XML_Error result = initializeEncoding(parser);
  if (result != XML_ERROR_NONE)
    return result;
  parser->m_processor = prologProcessor;
  return prologProcessor(parser, s, end, nextPtr);
}

#if defined(XML_DTD)

static enum XML_Error PTRCALL
externalParEntInitProcessor(XML_Parser parser, const char *s, const char *end,
                            const char **nextPtr) {
  enum XML_Error result = initializeEncoding(parser);
  if (result != XML_ERROR_NONE)
    return result;

  parser->m_dtd->paramEntityRead = XML_TRUE;

  if (parser->m_prologState.inEntityValue) {
    parser->m_processor = entityValueInitProcessor;
    return entityValueInitProcessor(parser, s, end, nextPtr);
  } else {
    parser->m_processor = externalParEntProcessor;
    return externalParEntProcessor(parser, s, end, nextPtr);
  }
}

static enum XML_Error PTRCALL
entityValueInitProcessor(XML_Parser parser, const char *s, const char *end,
                         const char **nextPtr) {
  int tok;
  const char *start = s;
  const char *next = start;
  parser->m_eventPtr = start;

  for (;;) {
    tok = XmlPrologTok(parser->m_encoding, start, end, &next);
    parser->m_eventEndPtr = next;
    if (tok <= 0) {
      if (! parser->m_parsingStatus.finalBuffer && tok != XML_TOK_INVALID) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      switch (tok) {
      case XML_TOK_INVALID:
        return XML_ERROR_INVALID_TOKEN;
      case XML_TOK_PARTIAL:
        return XML_ERROR_UNCLOSED_TOKEN;
      case XML_TOK_PARTIAL_CHAR:
        return XML_ERROR_PARTIAL_CHAR;
      case XML_TOK_NONE: 
      default:
        break;
      }
      return storeEntityValue(parser, parser->m_encoding, s, end,
                              XML_ACCOUNT_DIRECT, NULL);
    } else if (tok == XML_TOK_XML_DECL) {
      enum XML_Error result;
      result = processXmlDecl(parser, 0, start, next);
      if (result != XML_ERROR_NONE)
        return result;
      if (parser->m_parsingStatus.parsing == XML_FINISHED)
        return XML_ERROR_ABORTED;
      *nextPtr = next;
      parser->m_processor = entityValueProcessor;
      return entityValueProcessor(parser, next, end, nextPtr);
    }
    else if (tok == XML_TOK_BOM) {
#if XML_GE == 1
      if (! accountingDiffTolerated(parser, tok, s, next, __LINE__,
                                    XML_ACCOUNT_DIRECT)) {
        accountingOnAbort(parser);
        return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
      }
#endif

      *nextPtr = next;
      s = next;
    }
    else if (tok == XML_TOK_INSTANCE_START) {
      *nextPtr = next;
      return XML_ERROR_SYNTAX;
    }
    start = next;
    parser->m_eventPtr = start;
  }
}

static enum XML_Error PTRCALL
externalParEntProcessor(XML_Parser parser, const char *s, const char *end,
                        const char **nextPtr) {
  const char *next = s;
  int tok;

  tok = XmlPrologTok(parser->m_encoding, s, end, &next);
  if (tok <= 0) {
    if (! parser->m_parsingStatus.finalBuffer && tok != XML_TOK_INVALID) {
      *nextPtr = s;
      return XML_ERROR_NONE;
    }
    switch (tok) {
    case XML_TOK_INVALID:
      return XML_ERROR_INVALID_TOKEN;
    case XML_TOK_PARTIAL:
      return XML_ERROR_UNCLOSED_TOKEN;
    case XML_TOK_PARTIAL_CHAR:
      return XML_ERROR_PARTIAL_CHAR;
    case XML_TOK_NONE: 
    default:
      break;
    }
  }
  else if (tok == XML_TOK_BOM) {
    if (! accountingDiffTolerated(parser, tok, s, next, __LINE__,
                                  XML_ACCOUNT_DIRECT)) {
      accountingOnAbort(parser);
      return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
    }

    s = next;
    tok = XmlPrologTok(parser->m_encoding, s, end, &next);
  }

  parser->m_processor = prologProcessor;
  return doProlog(parser, parser->m_encoding, s, end, tok, next, nextPtr,
                  (XML_Bool)! parser->m_parsingStatus.finalBuffer, XML_TRUE,
                  XML_ACCOUNT_DIRECT);
}

static enum XML_Error PTRCALL
entityValueProcessor(XML_Parser parser, const char *s, const char *end,
                     const char **nextPtr) {
  const char *start = s;
  const char *next = s;
  const ENCODING *enc = parser->m_encoding;
  int tok;

  for (;;) {
    tok = XmlPrologTok(enc, start, end, &next);
    if (tok <= 0) {
      if (! parser->m_parsingStatus.finalBuffer && tok != XML_TOK_INVALID) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      switch (tok) {
      case XML_TOK_INVALID:
        return XML_ERROR_INVALID_TOKEN;
      case XML_TOK_PARTIAL:
        return XML_ERROR_UNCLOSED_TOKEN;
      case XML_TOK_PARTIAL_CHAR:
        return XML_ERROR_PARTIAL_CHAR;
      case XML_TOK_NONE: 
      default:
        break;
      }
      return storeEntityValue(parser, enc, s, end, XML_ACCOUNT_DIRECT, NULL);
    }
    else if (tok == XML_TOK_INSTANCE_START) {
      *nextPtr = next;
      return XML_ERROR_SYNTAX;
    }

    start = next;
  }
}

#endif

static enum XML_Error PTRCALL
prologProcessor(XML_Parser parser, const char *s, const char *end,
                const char **nextPtr) {
  const char *next = s;
  int tok = XmlPrologTok(parser->m_encoding, s, end, &next);
  return doProlog(parser, parser->m_encoding, s, end, tok, next, nextPtr,
                  (XML_Bool)! parser->m_parsingStatus.finalBuffer, XML_TRUE,
                  XML_ACCOUNT_DIRECT);
}

static enum XML_Error
doProlog(XML_Parser parser, const ENCODING *enc, const char *s, const char *end,
         int tok, const char *next, const char **nextPtr, XML_Bool haveMore,
         XML_Bool allowClosingDoctype, enum XML_Account account) {
#if defined(XML_DTD)
  static const XML_Char externalSubsetName[] = {ASCII_HASH, '\0'};
#endif
  static const XML_Char atypeCDATA[]
      = {ASCII_C, ASCII_D, ASCII_A, ASCII_T, ASCII_A, '\0'};
  static const XML_Char atypeID[] = {ASCII_I, ASCII_D, '\0'};
  static const XML_Char atypeIDREF[]
      = {ASCII_I, ASCII_D, ASCII_R, ASCII_E, ASCII_F, '\0'};
  static const XML_Char atypeIDREFS[]
      = {ASCII_I, ASCII_D, ASCII_R, ASCII_E, ASCII_F, ASCII_S, '\0'};
  static const XML_Char atypeENTITY[]
      = {ASCII_E, ASCII_N, ASCII_T, ASCII_I, ASCII_T, ASCII_Y, '\0'};
  static const XML_Char atypeENTITIES[]
      = {ASCII_E, ASCII_N, ASCII_T, ASCII_I, ASCII_T,
         ASCII_I, ASCII_E, ASCII_S, '\0'};
  static const XML_Char atypeNMTOKEN[]
      = {ASCII_N, ASCII_M, ASCII_T, ASCII_O, ASCII_K, ASCII_E, ASCII_N, '\0'};
  static const XML_Char atypeNMTOKENS[]
      = {ASCII_N, ASCII_M, ASCII_T, ASCII_O, ASCII_K,
         ASCII_E, ASCII_N, ASCII_S, '\0'};
  static const XML_Char notationPrefix[]
      = {ASCII_N, ASCII_O, ASCII_T, ASCII_A,      ASCII_T,
         ASCII_I, ASCII_O, ASCII_N, ASCII_LPAREN, '\0'};
  static const XML_Char enumValueSep[] = {ASCII_PIPE, '\0'};
  static const XML_Char enumValueStart[] = {ASCII_LPAREN, '\0'};

#if !defined(XML_DTD)
  UNUSED_P(account);
#endif

  DTD *const dtd = parser->m_dtd;

  const char **eventPP;
  const char **eventEndPP;
  enum XML_Content_Quant quant;

  if (enc == parser->m_encoding) {
    eventPP = &parser->m_eventPtr;
    eventEndPP = &parser->m_eventEndPtr;
  } else {
    eventPP = &(parser->m_openInternalEntities->internalEventPtr);
    eventEndPP = &(parser->m_openInternalEntities->internalEventEndPtr);
  }

  for (;;) {
    int role;
    XML_Bool handleDefault = XML_TRUE;
    *eventPP = s;
    *eventEndPP = next;
    if (tok <= 0) {
      if (haveMore && tok != XML_TOK_INVALID) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      switch (tok) {
      case XML_TOK_INVALID:
        *eventPP = next;
        return XML_ERROR_INVALID_TOKEN;
      case XML_TOK_PARTIAL:
        return XML_ERROR_UNCLOSED_TOKEN;
      case XML_TOK_PARTIAL_CHAR:
        return XML_ERROR_PARTIAL_CHAR;
      case -XML_TOK_PROLOG_S:
        tok = -tok;
        break;
      case XML_TOK_NONE:
#if defined(XML_DTD)
        if (enc != parser->m_encoding
            && ! parser->m_openInternalEntities->betweenDecl) {
          *nextPtr = s;
          return XML_ERROR_NONE;
        }
        if (parser->m_isParamEntity || enc != parser->m_encoding) {
          if (XmlTokenRole(&parser->m_prologState, XML_TOK_NONE, end, end, enc)
              == XML_ROLE_ERROR)
            return XML_ERROR_INCOMPLETE_PE;
          *nextPtr = s;
          return XML_ERROR_NONE;
        }
#endif
        return XML_ERROR_NO_ELEMENTS;
      default:
        tok = -tok;
        next = end;
        break;
      }
    }
    role = XmlTokenRole(&parser->m_prologState, tok, s, next, enc);
#if XML_GE == 1
    switch (role) {
    case XML_ROLE_INSTANCE_START: 
    case XML_ROLE_XML_DECL:       
#if defined(XML_DTD)
    case XML_ROLE_TEXT_DECL: 
#endif
      break;
    default:
      if (! accountingDiffTolerated(parser, tok, s, next, __LINE__, account)) {
        accountingOnAbort(parser);
        return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
      }
    }
#endif
    switch (role) {
    case XML_ROLE_XML_DECL: {
      enum XML_Error result = processXmlDecl(parser, 0, s, next);
      if (result != XML_ERROR_NONE)
        return result;
      enc = parser->m_encoding;
      handleDefault = XML_FALSE;
    } break;
    case XML_ROLE_DOCTYPE_NAME:
      if (parser->m_startDoctypeDeclHandler) {
        parser->m_doctypeName
            = poolStoreString(&parser->m_tempPool, enc, s, next);
        if (! parser->m_doctypeName)
          return XML_ERROR_NO_MEMORY;
        poolFinish(&parser->m_tempPool);
        parser->m_doctypePubid = NULL;
        handleDefault = XML_FALSE;
      }
      parser->m_doctypeSysid = NULL; 
      break;
    case XML_ROLE_DOCTYPE_INTERNAL_SUBSET:
      if (parser->m_startDoctypeDeclHandler) {
        beforeHandler(parser);
        parser->m_startDoctypeDeclHandler(
            parser->m_handlerArg, parser->m_doctypeName, parser->m_doctypeSysid,
            parser->m_doctypePubid, 1);
        afterHandler(parser);
        parser->m_doctypeName = NULL;
        poolClear(&parser->m_tempPool);
        handleDefault = XML_FALSE;
      }
      break;
#if defined(XML_DTD)
    case XML_ROLE_TEXT_DECL: {
      enum XML_Error result = processXmlDecl(parser, 1, s, next);
      if (result != XML_ERROR_NONE)
        return result;
      enc = parser->m_encoding;
      handleDefault = XML_FALSE;
    } break;
#endif
    case XML_ROLE_DOCTYPE_PUBLIC_ID:
#if defined(XML_DTD)
      parser->m_useForeignDTD = XML_FALSE;
      parser->m_declEntity = (ENTITY *)lookup(
          parser, &dtd->paramEntities, externalSubsetName, sizeof(ENTITY));
      if (! parser->m_declEntity)
        return XML_ERROR_NO_MEMORY;
#endif
      dtd->hasParamEntityRefs = XML_TRUE;
      if (parser->m_startDoctypeDeclHandler) {
        XML_Char *pubId;
        if (! XmlIsPublicId(enc, s, next, eventPP))
          return XML_ERROR_PUBLICID;
        pubId = poolStoreString(&parser->m_tempPool, enc,
                                s + enc->minBytesPerChar,
                                next - enc->minBytesPerChar);
        if (! pubId)
          return XML_ERROR_NO_MEMORY;
        normalizePublicId(pubId);
        poolFinish(&parser->m_tempPool);
        parser->m_doctypePubid = pubId;
        handleDefault = XML_FALSE;
        goto alreadyChecked;
      }
      EXPAT_FALLTHROUGH;
    case XML_ROLE_ENTITY_PUBLIC_ID:
      if (! XmlIsPublicId(enc, s, next, eventPP))
        return XML_ERROR_PUBLICID;
    alreadyChecked:
      if (dtd->keepProcessing && parser->m_declEntity) {
        XML_Char *tem
            = poolStoreString(&dtd->pool, enc, s + enc->minBytesPerChar,
                              next - enc->minBytesPerChar);
        if (! tem)
          return XML_ERROR_NO_MEMORY;
        normalizePublicId(tem);
        parser->m_declEntity->publicId = tem;
        poolFinish(&dtd->pool);
        if (parser->m_entityDeclHandler && role == XML_ROLE_ENTITY_PUBLIC_ID)
          handleDefault = XML_FALSE;
      }
      break;
    case XML_ROLE_DOCTYPE_CLOSE:
      if (allowClosingDoctype != XML_TRUE) {
        return XML_ERROR_INVALID_TOKEN;
      }

      if (parser->m_doctypeName) {
        beforeHandler(parser);
        parser->m_startDoctypeDeclHandler(
            parser->m_handlerArg, parser->m_doctypeName, parser->m_doctypeSysid,
            parser->m_doctypePubid, 0);
        afterHandler(parser);
        poolClear(&parser->m_tempPool);
        handleDefault = XML_FALSE;
      }
#if defined(XML_DTD)
      if (parser->m_doctypeSysid || parser->m_useForeignDTD) {
        XML_Bool hadParamEntityRefs = dtd->hasParamEntityRefs;
        dtd->hasParamEntityRefs = XML_TRUE;
        if (parser->m_paramEntityParsing
            && parser->m_externalEntityRefHandler) {
          ENTITY *entity = (ENTITY *)lookup(parser, &dtd->paramEntities,
                                            externalSubsetName, sizeof(ENTITY));
          if (! entity) {
            return XML_ERROR_NO_MEMORY; /* LCOV_EXCL_LINE */
          }
          if (parser->m_useForeignDTD)
            entity->base = parser->m_curBase;
          dtd->paramEntityRead = XML_FALSE;
          beforeHandler(parser);
          const int status = parser->m_externalEntityRefHandler(
              parser->m_externalEntityRefHandlerArg, 0, entity->base,
              entity->systemId, entity->publicId);
          afterHandler(parser);
          if (! status)
            return XML_ERROR_EXTERNAL_ENTITY_HANDLING;
          if (dtd->paramEntityRead) {
            if (! dtd->standalone && parser->m_notStandaloneHandler) {
              beforeHandler(parser);
              const int handlerStatus
                  = parser->m_notStandaloneHandler(parser->m_handlerArg);
              afterHandler(parser);
              if (! handlerStatus)
                return XML_ERROR_NOT_STANDALONE;
            }
          }
          else if (! parser->m_doctypeSysid)
            dtd->hasParamEntityRefs = hadParamEntityRefs;
        }
        parser->m_useForeignDTD = XML_FALSE;
      }
#endif
      if (parser->m_endDoctypeDeclHandler) {
        beforeHandler(parser);
        parser->m_endDoctypeDeclHandler(parser->m_handlerArg);
        afterHandler(parser);
        handleDefault = XML_FALSE;
      }
      break;
    case XML_ROLE_INSTANCE_START:
#if defined(XML_DTD)
      if (parser->m_useForeignDTD) {
        XML_Bool hadParamEntityRefs = dtd->hasParamEntityRefs;
        dtd->hasParamEntityRefs = XML_TRUE;
        if (parser->m_paramEntityParsing
            && parser->m_externalEntityRefHandler) {
          ENTITY *entity = (ENTITY *)lookup(parser, &dtd->paramEntities,
                                            externalSubsetName, sizeof(ENTITY));
          if (! entity)
            return XML_ERROR_NO_MEMORY;
          entity->base = parser->m_curBase;
          dtd->paramEntityRead = XML_FALSE;
          beforeHandler(parser);
          const int status = parser->m_externalEntityRefHandler(
              parser->m_externalEntityRefHandlerArg, 0, entity->base,
              entity->systemId, entity->publicId);
          afterHandler(parser);
          if (! status)
            return XML_ERROR_EXTERNAL_ENTITY_HANDLING;
          if (dtd->paramEntityRead) {
            if (! dtd->standalone && parser->m_notStandaloneHandler) {
              beforeHandler(parser);
              const int handlerStatus
                  = parser->m_notStandaloneHandler(parser->m_handlerArg);
              afterHandler(parser);
              if (! handlerStatus)
                return XML_ERROR_NOT_STANDALONE;
            }
          }
          else
            dtd->hasParamEntityRefs = hadParamEntityRefs;
        }
      }
#endif
      parser->m_processor = contentProcessor;
      return contentProcessor(parser, s, end, nextPtr);
    case XML_ROLE_ATTLIST_ELEMENT_NAME:
      parser->m_declElementType = getElementType(parser, enc, s, next);
      if (! parser->m_declElementType)
        return XML_ERROR_NO_MEMORY;
      goto checkAttListDeclHandler;
    case XML_ROLE_ATTRIBUTE_NAME:
      parser->m_declAttributeId = getAttributeId(parser, enc, s, next);
      if (! parser->m_declAttributeId)
        return XML_ERROR_NO_MEMORY;
      parser->m_declAttributeIsCdata = XML_FALSE;
      parser->m_declAttributeType = NULL;
      parser->m_declAttributeIsId = XML_FALSE;
      goto checkAttListDeclHandler;
    case XML_ROLE_ATTRIBUTE_TYPE_CDATA:
      parser->m_declAttributeIsCdata = XML_TRUE;
      parser->m_declAttributeType = atypeCDATA;
      goto checkAttListDeclHandler;
    case XML_ROLE_ATTRIBUTE_TYPE_ID:
      parser->m_declAttributeIsId = XML_TRUE;
      parser->m_declAttributeType = atypeID;
      goto checkAttListDeclHandler;
    case XML_ROLE_ATTRIBUTE_TYPE_IDREF:
      parser->m_declAttributeType = atypeIDREF;
      goto checkAttListDeclHandler;
    case XML_ROLE_ATTRIBUTE_TYPE_IDREFS:
      parser->m_declAttributeType = atypeIDREFS;
      goto checkAttListDeclHandler;
    case XML_ROLE_ATTRIBUTE_TYPE_ENTITY:
      parser->m_declAttributeType = atypeENTITY;
      goto checkAttListDeclHandler;
    case XML_ROLE_ATTRIBUTE_TYPE_ENTITIES:
      parser->m_declAttributeType = atypeENTITIES;
      goto checkAttListDeclHandler;
    case XML_ROLE_ATTRIBUTE_TYPE_NMTOKEN:
      parser->m_declAttributeType = atypeNMTOKEN;
      goto checkAttListDeclHandler;
    case XML_ROLE_ATTRIBUTE_TYPE_NMTOKENS:
      parser->m_declAttributeType = atypeNMTOKENS;
    checkAttListDeclHandler:
      if (dtd->keepProcessing && parser->m_attlistDeclHandler)
        handleDefault = XML_FALSE;
      break;
    case XML_ROLE_ATTRIBUTE_ENUM_VALUE:
    case XML_ROLE_ATTRIBUTE_NOTATION_VALUE:
      if (dtd->keepProcessing && parser->m_attlistDeclHandler) {
        const XML_Char *prefix;
        if (parser->m_declAttributeType) {
          prefix = enumValueSep;
        } else {
          prefix = (role == XML_ROLE_ATTRIBUTE_NOTATION_VALUE ? notationPrefix
                                                              : enumValueStart);
        }
        if (! poolAppendString(&parser->m_tempPool, prefix))
          return XML_ERROR_NO_MEMORY;
        if (! poolAppend(&parser->m_tempPool, enc, s, next))
          return XML_ERROR_NO_MEMORY;
        parser->m_declAttributeType = parser->m_tempPool.start;
        handleDefault = XML_FALSE;
      }
      break;
    case XML_ROLE_IMPLIED_ATTRIBUTE_VALUE:
    case XML_ROLE_REQUIRED_ATTRIBUTE_VALUE:
      if (dtd->keepProcessing) {
        if (! defineAttribute(parser->m_declElementType,
                              parser->m_declAttributeId,
                              parser->m_declAttributeIsCdata,
                              parser->m_declAttributeIsId, 0, parser))
          return XML_ERROR_NO_MEMORY;
        if (parser->m_attlistDeclHandler && parser->m_declAttributeType) {
          if (*parser->m_declAttributeType == XML_T(ASCII_LPAREN)
              || (*parser->m_declAttributeType == XML_T(ASCII_N)
                  && parser->m_declAttributeType[1] == XML_T(ASCII_O))) {
            if (! poolAppendChar(&parser->m_tempPool, XML_T(ASCII_RPAREN))
                || ! poolAppendChar(&parser->m_tempPool, XML_T('\0')))
              return XML_ERROR_NO_MEMORY;
            parser->m_declAttributeType = parser->m_tempPool.start;
            poolFinish(&parser->m_tempPool);
          }
          *eventEndPP = s;
          beforeHandler(parser);
          parser->m_attlistDeclHandler(
              parser->m_handlerArg, parser->m_declElementType->name,
              parser->m_declAttributeId->name, parser->m_declAttributeType, 0,
              role == XML_ROLE_REQUIRED_ATTRIBUTE_VALUE);
          afterHandler(parser);
          handleDefault = XML_FALSE;
        }
      }
      poolClear(&parser->m_tempPool);
      break;
    case XML_ROLE_DEFAULT_ATTRIBUTE_VALUE:
    case XML_ROLE_FIXED_ATTRIBUTE_VALUE:
      if (dtd->keepProcessing) {
        const XML_Char *attVal;
        enum XML_Error result = storeAttributeValue(
            parser, enc, parser->m_declAttributeIsCdata,
            s + enc->minBytesPerChar, next - enc->minBytesPerChar, &dtd->pool,
            XML_ACCOUNT_NONE);
        if (result)
          return result;
        attVal = poolStart(&dtd->pool);
        poolFinish(&dtd->pool);
        if (! defineAttribute(
                parser->m_declElementType, parser->m_declAttributeId,
                parser->m_declAttributeIsCdata, XML_FALSE, attVal, parser))
          return XML_ERROR_NO_MEMORY;
        if (parser->m_attlistDeclHandler && parser->m_declAttributeType) {
          if (*parser->m_declAttributeType == XML_T(ASCII_LPAREN)
              || (*parser->m_declAttributeType == XML_T(ASCII_N)
                  && parser->m_declAttributeType[1] == XML_T(ASCII_O))) {
            if (! poolAppendChar(&parser->m_tempPool, XML_T(ASCII_RPAREN))
                || ! poolAppendChar(&parser->m_tempPool, XML_T('\0')))
              return XML_ERROR_NO_MEMORY;
            parser->m_declAttributeType = parser->m_tempPool.start;
            poolFinish(&parser->m_tempPool);
          }
          *eventEndPP = s;
          beforeHandler(parser);
          parser->m_attlistDeclHandler(
              parser->m_handlerArg, parser->m_declElementType->name,
              parser->m_declAttributeId->name, parser->m_declAttributeType,
              attVal, role == XML_ROLE_FIXED_ATTRIBUTE_VALUE);
          afterHandler(parser);
          poolClear(&parser->m_tempPool);
          handleDefault = XML_FALSE;
        }
      }
      break;
    case XML_ROLE_ENTITY_VALUE:
      if (dtd->keepProcessing) {
#if XML_GE == 1
        enum XML_Error result = callStoreEntityValue(
            parser, enc, s + enc->minBytesPerChar, next - enc->minBytesPerChar,
            XML_ACCOUNT_NONE);
        if (parser->m_declEntity) {
          if ((size_t)poolLength(&dtd->entityValuePool) > (size_t)INT_MAX) {
            return XML_ERROR_NO_MEMORY;
          }
          parser->m_declEntity->textPtr = poolStart(&dtd->entityValuePool);
          parser->m_declEntity->textLen
              = (int)(poolLength(&dtd->entityValuePool));
          poolFinish(&dtd->entityValuePool);
          if (parser->m_entityDeclHandler) {
            *eventEndPP = s;
            beforeHandler(parser);
            parser->m_entityDeclHandler(
                parser->m_handlerArg, parser->m_declEntity->name,
                parser->m_declEntity->is_param, parser->m_declEntity->textPtr,
                parser->m_declEntity->textLen, parser->m_curBase, 0, 0, 0);
            afterHandler(parser);
            handleDefault = XML_FALSE;
          }
        } else
          poolDiscard(&dtd->entityValuePool);
        if (result != XML_ERROR_NONE)
          return result;
#else
        if (parser->m_declEntity != NULL) {
          const enum XML_Error result
              = storeSelfEntityValue(parser, parser->m_declEntity);
          if (result != XML_ERROR_NONE)
            return result;

          if (parser->m_entityDeclHandler) {
            *eventEndPP = s;
            beforeHandler(parser);
            parser->m_entityDeclHandler(
                parser->m_handlerArg, parser->m_declEntity->name,
                parser->m_declEntity->is_param, parser->m_declEntity->textPtr,
                parser->m_declEntity->textLen, parser->m_curBase, 0, 0, 0);
            afterHandler(parser);
            handleDefault = XML_FALSE;
          }
        }
#endif
      }
      break;
    case XML_ROLE_DOCTYPE_SYSTEM_ID:
#if defined(XML_DTD)
      parser->m_useForeignDTD = XML_FALSE;
#endif
      dtd->hasParamEntityRefs = XML_TRUE;
      if (parser->m_startDoctypeDeclHandler) {
        parser->m_doctypeSysid = poolStoreString(&parser->m_tempPool, enc,
                                                 s + enc->minBytesPerChar,
                                                 next - enc->minBytesPerChar);
        if (parser->m_doctypeSysid == NULL)
          return XML_ERROR_NO_MEMORY;
        poolFinish(&parser->m_tempPool);
        handleDefault = XML_FALSE;
      }
#if defined(XML_DTD)
      else
        parser->m_doctypeSysid = externalSubsetName;
#endif
      if (! dtd->standalone
#if defined(XML_DTD)
          && ! parser->m_paramEntityParsing
#endif
          && parser->m_notStandaloneHandler) {
        beforeHandler(parser);
        const int status = parser->m_notStandaloneHandler(parser->m_handlerArg);
        afterHandler(parser);
        if (! status)
          return XML_ERROR_NOT_STANDALONE;
      }
#if !defined(XML_DTD)
      break;
#else
      if (! parser->m_declEntity) {
        parser->m_declEntity = (ENTITY *)lookup(
            parser, &dtd->paramEntities, externalSubsetName, sizeof(ENTITY));
        if (! parser->m_declEntity)
          return XML_ERROR_NO_MEMORY;
        parser->m_declEntity->publicId = NULL;
      }
#endif
      EXPAT_FALLTHROUGH;
    case XML_ROLE_ENTITY_SYSTEM_ID:
      if (dtd->keepProcessing && parser->m_declEntity) {
        parser->m_declEntity->systemId
            = poolStoreString(&dtd->pool, enc, s + enc->minBytesPerChar,
                              next - enc->minBytesPerChar);
        if (! parser->m_declEntity->systemId)
          return XML_ERROR_NO_MEMORY;
        parser->m_declEntity->base = parser->m_curBase;
        poolFinish(&dtd->pool);
        if (parser->m_entityDeclHandler && role == XML_ROLE_ENTITY_SYSTEM_ID)
          handleDefault = XML_FALSE;
      }
      break;
    case XML_ROLE_ENTITY_COMPLETE:
#if XML_GE == 0
      if (parser->m_declEntity != NULL) {
        const enum XML_Error result
            = storeSelfEntityValue(parser, parser->m_declEntity);
        if (result != XML_ERROR_NONE)
          return result;
      }
#endif
      if (dtd->keepProcessing && parser->m_declEntity
          && parser->m_entityDeclHandler) {
        *eventEndPP = s;
        beforeHandler(parser);
        parser->m_entityDeclHandler(
            parser->m_handlerArg, parser->m_declEntity->name,
            parser->m_declEntity->is_param, 0, 0, parser->m_declEntity->base,
            parser->m_declEntity->systemId, parser->m_declEntity->publicId, 0);
        afterHandler(parser);
        handleDefault = XML_FALSE;
      }
      break;
    case XML_ROLE_ENTITY_NOTATION_NAME:
      if (dtd->keepProcessing && parser->m_declEntity) {
        parser->m_declEntity->notation
            = poolStoreString(&dtd->pool, enc, s, next);
        if (! parser->m_declEntity->notation)
          return XML_ERROR_NO_MEMORY;
        poolFinish(&dtd->pool);
        if (parser->m_unparsedEntityDeclHandler) {
          *eventEndPP = s;
          beforeHandler(parser);
          parser->m_unparsedEntityDeclHandler(
              parser->m_handlerArg, parser->m_declEntity->name,
              parser->m_declEntity->base, parser->m_declEntity->systemId,
              parser->m_declEntity->publicId, parser->m_declEntity->notation);
          afterHandler(parser);
          handleDefault = XML_FALSE;
        } else if (parser->m_entityDeclHandler) {
          *eventEndPP = s;
          beforeHandler(parser);
          parser->m_entityDeclHandler(
              parser->m_handlerArg, parser->m_declEntity->name, 0, 0, 0,
              parser->m_declEntity->base, parser->m_declEntity->systemId,
              parser->m_declEntity->publicId, parser->m_declEntity->notation);
          afterHandler(parser);
          handleDefault = XML_FALSE;
        }
      }
      break;
    case XML_ROLE_GENERAL_ENTITY_NAME: {
      if (XmlPredefinedEntityName(enc, s, next)) {
        parser->m_declEntity = NULL;
        break;
      }
      if (dtd->keepProcessing) {
        const XML_Char *name = poolStoreString(&dtd->pool, enc, s, next);
        if (! name)
          return XML_ERROR_NO_MEMORY;
        parser->m_declEntity = (ENTITY *)lookup(parser, &dtd->generalEntities,
                                                name, sizeof(ENTITY));
        if (! parser->m_declEntity)
          return XML_ERROR_NO_MEMORY;
        if (parser->m_declEntity->name != name) {
          poolDiscard(&dtd->pool);
          parser->m_declEntity = NULL;
        } else {
          poolFinish(&dtd->pool);
          parser->m_declEntity->publicId = NULL;
          parser->m_declEntity->is_param = XML_FALSE;
          parser->m_declEntity->is_internal
              = ! (parser->m_parentParser || parser->m_openInternalEntities);
          if (parser->m_entityDeclHandler)
            handleDefault = XML_FALSE;
        }
      } else {
        poolDiscard(&dtd->pool);
        parser->m_declEntity = NULL;
      }
    } break;
    case XML_ROLE_PARAM_ENTITY_NAME:
#if defined(XML_DTD)
      if (dtd->keepProcessing) {
        const XML_Char *name = poolStoreString(&dtd->pool, enc, s, next);
        if (! name)
          return XML_ERROR_NO_MEMORY;
        parser->m_declEntity = (ENTITY *)lookup(parser, &dtd->paramEntities,
                                                name, sizeof(ENTITY));
        if (! parser->m_declEntity)
          return XML_ERROR_NO_MEMORY;
        if (parser->m_declEntity->name != name) {
          poolDiscard(&dtd->pool);
          parser->m_declEntity = NULL;
        } else {
          poolFinish(&dtd->pool);
          parser->m_declEntity->publicId = NULL;
          parser->m_declEntity->is_param = XML_TRUE;
          parser->m_declEntity->is_internal
              = ! (parser->m_parentParser || parser->m_openInternalEntities);
          if (parser->m_entityDeclHandler)
            handleDefault = XML_FALSE;
        }
      } else {
        poolDiscard(&dtd->pool);
        parser->m_declEntity = NULL;
      }
#else
      parser->m_declEntity = NULL;
#endif
      break;
    case XML_ROLE_NOTATION_NAME:
      parser->m_declNotationPublicId = NULL;
      parser->m_declNotationName = NULL;
      if (parser->m_notationDeclHandler) {
        parser->m_declNotationName
            = poolStoreString(&parser->m_tempPool, enc, s, next);
        if (! parser->m_declNotationName)
          return XML_ERROR_NO_MEMORY;
        poolFinish(&parser->m_tempPool);
        handleDefault = XML_FALSE;
      }
      break;
    case XML_ROLE_NOTATION_PUBLIC_ID:
      if (! XmlIsPublicId(enc, s, next, eventPP))
        return XML_ERROR_PUBLICID;
      if (parser
              ->m_declNotationName) { 
        XML_Char *tem = poolStoreString(&parser->m_tempPool, enc,
                                        s + enc->minBytesPerChar,
                                        next - enc->minBytesPerChar);
        if (! tem)
          return XML_ERROR_NO_MEMORY;
        normalizePublicId(tem);
        parser->m_declNotationPublicId = tem;
        poolFinish(&parser->m_tempPool);
        handleDefault = XML_FALSE;
      }
      break;
    case XML_ROLE_NOTATION_SYSTEM_ID:
      if (parser->m_declNotationName && parser->m_notationDeclHandler) {
        const XML_Char *systemId = poolStoreString(&parser->m_tempPool, enc,
                                                   s + enc->minBytesPerChar,
                                                   next - enc->minBytesPerChar);
        if (! systemId)
          return XML_ERROR_NO_MEMORY;
        *eventEndPP = s;
        beforeHandler(parser);
        parser->m_notationDeclHandler(
            parser->m_handlerArg, parser->m_declNotationName, parser->m_curBase,
            systemId, parser->m_declNotationPublicId);
        afterHandler(parser);
        handleDefault = XML_FALSE;
      }
      poolClear(&parser->m_tempPool);
      break;
    case XML_ROLE_NOTATION_NO_SYSTEM_ID:
      if (parser->m_declNotationPublicId && parser->m_notationDeclHandler) {
        *eventEndPP = s;
        beforeHandler(parser);
        parser->m_notationDeclHandler(
            parser->m_handlerArg, parser->m_declNotationName, parser->m_curBase,
            0, parser->m_declNotationPublicId);
        afterHandler(parser);
        handleDefault = XML_FALSE;
      }
      poolClear(&parser->m_tempPool);
      break;
    case XML_ROLE_ERROR:
      switch (tok) {
      case XML_TOK_PARAM_ENTITY_REF:
        return XML_ERROR_PARAM_ENTITY_REF;
      case XML_TOK_XML_DECL:
        return XML_ERROR_MISPLACED_XML_PI;
      default:
        return XML_ERROR_SYNTAX;
      }
#if defined(XML_DTD)
    case XML_ROLE_IGNORE_SECT: {
      enum XML_Error result;
      if (parser->m_defaultHandler)
        reportDefault(parser, enc, s, next);
      handleDefault = XML_FALSE;
      result = doIgnoreSection(parser, enc, &next, end, nextPtr, haveMore);
      if (result != XML_ERROR_NONE)
        return result;
      else if (! next) {
        parser->m_processor = ignoreSectionProcessor;
        return result;
      }
    } break;
#endif
    case XML_ROLE_GROUP_OPEN:
      if (parser->m_prologState.level >= parser->m_groupSize) {
        if (parser->m_groupSize) {
          if (parser->m_groupSize > SIZE_MAX / 2) {
            return XML_ERROR_NO_MEMORY;
          }

          char *const new_connector = REALLOC(parser, parser->m_groupConnector,
                                              parser->m_groupSize *= 2);
          if (new_connector == NULL) {
            parser->m_groupSize /= 2;
            return XML_ERROR_NO_MEMORY;
          }
          parser->m_groupConnector = new_connector;
        } else {
          parser->m_groupConnector = MALLOC(parser, parser->m_groupSize = 32);
          if (! parser->m_groupConnector) {
            parser->m_groupSize = 0;
            return XML_ERROR_NO_MEMORY;
          }
        }
      }
      parser->m_groupConnector[parser->m_prologState.level] = 0;
      if (dtd->in_eldecl) {
        int myindex = nextScaffoldPart(parser);
        if (myindex < 0)
          return XML_ERROR_NO_MEMORY;
        assert(dtd->scaffIndex != NULL);
        if ((size_t)dtd->scaffLevel >= dtd->scaffIndexSize) {
          if (dtd->scaffIndexSize > SIZE_MAX / 2 / sizeof(int)) {
            return XML_ERROR_NO_MEMORY;
          }
          assert(dtd->scaffIndexSize > 0);
          const size_t new_size = dtd->scaffIndexSize * 2;
          int *const new_scaff_index
              = REALLOC(parser, dtd->scaffIndex, new_size * sizeof(int));
          if (new_scaff_index == NULL) {
            return XML_ERROR_NO_MEMORY;
          }
          dtd->scaffIndex = new_scaff_index;
          dtd->scaffIndexSize = new_size;
        }
        dtd->scaffIndex[dtd->scaffLevel] = myindex;
        dtd->scaffLevel++;
        dtd->scaffold[myindex].type = XML_CTYPE_SEQ;
        if (parser->m_elementDeclHandler)
          handleDefault = XML_FALSE;
      }
      break;
    case XML_ROLE_GROUP_SEQUENCE:
      if (parser->m_groupConnector[parser->m_prologState.level] == ASCII_PIPE)
        return XML_ERROR_SYNTAX;
      parser->m_groupConnector[parser->m_prologState.level] = ASCII_COMMA;
      if (dtd->in_eldecl && parser->m_elementDeclHandler)
        handleDefault = XML_FALSE;
      break;
    case XML_ROLE_GROUP_CHOICE:
      if (parser->m_groupConnector[parser->m_prologState.level] == ASCII_COMMA)
        return XML_ERROR_SYNTAX;
      if (dtd->in_eldecl
          && ! parser->m_groupConnector[parser->m_prologState.level]
          && (dtd->scaffold[dtd->scaffIndex[dtd->scaffLevel - 1]].type
              != XML_CTYPE_MIXED)) {
        dtd->scaffold[dtd->scaffIndex[dtd->scaffLevel - 1]].type
            = XML_CTYPE_CHOICE;
        if (parser->m_elementDeclHandler)
          handleDefault = XML_FALSE;
      }
      parser->m_groupConnector[parser->m_prologState.level] = ASCII_PIPE;
      break;
    case XML_ROLE_PARAM_ENTITY_REF:
#if defined(XML_DTD)
    case XML_ROLE_INNER_PARAM_ENTITY_REF:
      dtd->hasParamEntityRefs = XML_TRUE;
      if (! parser->m_paramEntityParsing)
        dtd->keepProcessing = dtd->standalone;
      else {
        const XML_Char *name;
        ENTITY *entity;
        name = poolStoreString(&dtd->pool, enc, s + enc->minBytesPerChar,
                               next - enc->minBytesPerChar);
        if (! name)
          return XML_ERROR_NO_MEMORY;
        entity = (ENTITY *)lookup(parser, &dtd->paramEntities, name, 0);
        poolDiscard(&dtd->pool);
        if (parser->m_prologState.documentEntity
            && (dtd->standalone ? ! parser->m_openInternalEntities
                                : ! dtd->hasParamEntityRefs)) {
          if (! entity)
            return XML_ERROR_UNDEFINED_ENTITY;
          else if (! entity->is_internal) {
            return XML_ERROR_ENTITY_DECLARED_IN_PE; /* LCOV_EXCL_LINE */
          }
        } else if (! entity) {
          dtd->keepProcessing = dtd->standalone;
          if ((role == XML_ROLE_PARAM_ENTITY_REF)
              && parser->m_skippedEntityHandler) {
            beforeHandler(parser);
            parser->m_skippedEntityHandler(parser->m_handlerArg, name, 1);
            afterHandler(parser);
            handleDefault = XML_FALSE;
          }
          break;
        }
        if (entity->open)
          return XML_ERROR_RECURSIVE_ENTITY_REF;
        if (entity->textPtr) {
          enum XML_Error result;
          XML_Bool betweenDecl
              = (role == XML_ROLE_PARAM_ENTITY_REF ? XML_TRUE : XML_FALSE);
          result = processEntity(parser, entity, betweenDecl, ENTITY_INTERNAL);
          if (result != XML_ERROR_NONE)
            return result;
          handleDefault = XML_FALSE;
          break;
        }
        if (parser->m_externalEntityRefHandler) {
          dtd->paramEntityRead = XML_FALSE;
          entity->open = XML_TRUE;
          entityTrackingOnOpen(parser, entity, __LINE__);
          beforeHandler(parser);
          const int status = parser->m_externalEntityRefHandler(
              parser->m_externalEntityRefHandlerArg, entity->name, entity->base,
              entity->systemId, entity->publicId);
          afterHandler(parser);
          if (! status) {
            entityTrackingOnClose(parser, entity, __LINE__);
            entity->open = XML_FALSE;
            return XML_ERROR_EXTERNAL_ENTITY_HANDLING;
          }
          entityTrackingOnClose(parser, entity, __LINE__);
          entity->open = XML_FALSE;
          handleDefault = XML_FALSE;
          if (! dtd->paramEntityRead) {
            dtd->keepProcessing = dtd->standalone;
            break;
          }
        } else {
          dtd->keepProcessing = dtd->standalone;
          break;
        }
      }
#endif
      if (! dtd->standalone && parser->m_notStandaloneHandler) {
        beforeHandler(parser);
        const int status = parser->m_notStandaloneHandler(parser->m_handlerArg);
        afterHandler(parser);
        if (! status)
          return XML_ERROR_NOT_STANDALONE;
      }
      break;


    case XML_ROLE_ELEMENT_NAME:
      if (parser->m_elementDeclHandler) {
        parser->m_declElementType = getElementType(parser, enc, s, next);
        if (! parser->m_declElementType)
          return XML_ERROR_NO_MEMORY;
        dtd->scaffLevel = 0;
        dtd->scaffCount = 0;
        dtd->in_eldecl = XML_TRUE;
        handleDefault = XML_FALSE;
      }
      break;

    case XML_ROLE_CONTENT_ANY:
    case XML_ROLE_CONTENT_EMPTY:
      if (dtd->in_eldecl) {
        if (parser->m_elementDeclHandler) {
          XML_Content *content = parser->m_mem.malloc_fcn(sizeof(XML_Content));
          if (! content)
            return XML_ERROR_NO_MEMORY;
          content->quant = XML_CQUANT_NONE;
          content->name = NULL;
          content->numchildren = 0;
          content->children = NULL;
          content->type = ((role == XML_ROLE_CONTENT_ANY) ? XML_CTYPE_ANY
                                                          : XML_CTYPE_EMPTY);
          *eventEndPP = s;
          beforeHandler(parser);
          parser->m_elementDeclHandler(
              parser->m_handlerArg, parser->m_declElementType->name, content);
          afterHandler(parser);
          handleDefault = XML_FALSE;
        }
        dtd->in_eldecl = XML_FALSE;
      }
      break;

    case XML_ROLE_CONTENT_PCDATA:
      if (dtd->in_eldecl) {
        dtd->scaffold[dtd->scaffIndex[dtd->scaffLevel - 1]].type
            = XML_CTYPE_MIXED;
        if (parser->m_elementDeclHandler)
          handleDefault = XML_FALSE;
      }
      break;

    case XML_ROLE_CONTENT_ELEMENT:
      quant = XML_CQUANT_NONE;
      goto elementContent;
    case XML_ROLE_CONTENT_ELEMENT_OPT:
      quant = XML_CQUANT_OPT;
      goto elementContent;
    case XML_ROLE_CONTENT_ELEMENT_REP:
      quant = XML_CQUANT_REP;
      goto elementContent;
    case XML_ROLE_CONTENT_ELEMENT_PLUS:
      quant = XML_CQUANT_PLUS;
    elementContent:
      if (dtd->in_eldecl) {
        ELEMENT_TYPE *el;
        const XML_Char *name;
        size_t nameLen;
        const char *nxt
            = (quant == XML_CQUANT_NONE ? next : next - enc->minBytesPerChar);
        int myindex = nextScaffoldPart(parser);
        if (myindex < 0)
          return XML_ERROR_NO_MEMORY;
        dtd->scaffold[myindex].type = XML_CTYPE_NAME;
        dtd->scaffold[myindex].quant = quant;
        el = getElementType(parser, enc, s, nxt);
        if (! el)
          return XML_ERROR_NO_MEMORY;
        name = el->name;
        dtd->scaffold[myindex].name = name;
        nameLen = xcslen(name) +  1;

        if (nameLen > UINT_MAX - dtd->contentStringLen) {
          return XML_ERROR_NO_MEMORY;
        }

        dtd->contentStringLen += (unsigned)nameLen;
        if (parser->m_elementDeclHandler)
          handleDefault = XML_FALSE;
      }
      break;

    case XML_ROLE_GROUP_CLOSE:
      quant = XML_CQUANT_NONE;
      goto closeGroup;
    case XML_ROLE_GROUP_CLOSE_OPT:
      quant = XML_CQUANT_OPT;
      goto closeGroup;
    case XML_ROLE_GROUP_CLOSE_REP:
      quant = XML_CQUANT_REP;
      goto closeGroup;
    case XML_ROLE_GROUP_CLOSE_PLUS:
      quant = XML_CQUANT_PLUS;
    closeGroup:
      if (dtd->in_eldecl) {
        if (parser->m_elementDeclHandler)
          handleDefault = XML_FALSE;
        dtd->scaffLevel--;
        dtd->scaffold[dtd->scaffIndex[dtd->scaffLevel]].quant = quant;
        if (dtd->scaffLevel == 0) {
          if (! handleDefault) {
            XML_Content *model = build_model(parser);
            if (! model)
              return XML_ERROR_NO_MEMORY;
            *eventEndPP = s;
            beforeHandler(parser);
            parser->m_elementDeclHandler(
                parser->m_handlerArg, parser->m_declElementType->name, model);
            afterHandler(parser);
          }
          dtd->in_eldecl = XML_FALSE;
          dtd->contentStringLen = 0;
        }
      }
      break;

    case XML_ROLE_PI:
      if (! reportProcessingInstruction(parser, enc, s, next))
        return XML_ERROR_NO_MEMORY;
      handleDefault = XML_FALSE;
      break;
    case XML_ROLE_COMMENT:
      if (! reportComment(parser, enc, s, next))
        return XML_ERROR_NO_MEMORY;
      handleDefault = XML_FALSE;
      break;
    case XML_ROLE_NONE:
      switch (tok) {
      case XML_TOK_BOM:
        handleDefault = XML_FALSE;
        break;
      }
      break;
    case XML_ROLE_DOCTYPE_NONE:
      if (parser->m_startDoctypeDeclHandler)
        handleDefault = XML_FALSE;
      break;
    case XML_ROLE_ENTITY_NONE:
      if (dtd->keepProcessing && parser->m_entityDeclHandler)
        handleDefault = XML_FALSE;
      break;
    case XML_ROLE_NOTATION_NONE:
      if (parser->m_notationDeclHandler)
        handleDefault = XML_FALSE;
      break;
    case XML_ROLE_ATTLIST_NONE:
      if (dtd->keepProcessing && parser->m_attlistDeclHandler)
        handleDefault = XML_FALSE;
      break;
    case XML_ROLE_ELEMENT_NONE:
      if (parser->m_elementDeclHandler)
        handleDefault = XML_FALSE;
      break;
    } 

    if (handleDefault && parser->m_defaultHandler)
      reportDefault(parser, enc, s, next);

    switch (parser->m_parsingStatus.parsing) {
    case XML_SUSPENDED:
      *nextPtr = next;
      return XML_ERROR_NONE;
    case XML_FINISHED:
      return XML_ERROR_ABORTED;
    case XML_PARSING:
      if (parser->m_reenter) {
        *nextPtr = next;
        return XML_ERROR_NONE;
      }
      EXPAT_FALLTHROUGH;
    default:
      s = next;
      tok = XmlPrologTok(enc, s, end, &next);
    }
  }
}

static enum XML_Error PTRCALL
epilogProcessor(XML_Parser parser, const char *s, const char *end,
                const char **nextPtr) {
  parser->m_processor = epilogProcessor;
  parser->m_eventPtr = s;
  for (;;) {
    const char *next = NULL;
    int tok = XmlPrologTok(parser->m_encoding, s, end, &next);
#if XML_GE == 1
    if (! accountingDiffTolerated(parser, tok, s, next, __LINE__,
                                  XML_ACCOUNT_DIRECT)) {
      accountingOnAbort(parser);
      return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
    }
#endif
    parser->m_eventEndPtr = next;
    switch (tok) {
    case -XML_TOK_PROLOG_S:
      if (parser->m_defaultHandler) {
        reportDefault(parser, parser->m_encoding, s, next);
        if (parser->m_parsingStatus.parsing == XML_FINISHED)
          return XML_ERROR_ABORTED;
      }
      *nextPtr = next;
      return XML_ERROR_NONE;
    case XML_TOK_NONE:
      *nextPtr = s;
      return XML_ERROR_NONE;
    case XML_TOK_PROLOG_S:
      if (parser->m_defaultHandler)
        reportDefault(parser, parser->m_encoding, s, next);
      break;
    case XML_TOK_PI:
      if (! reportProcessingInstruction(parser, parser->m_encoding, s, next))
        return XML_ERROR_NO_MEMORY;
      break;
    case XML_TOK_COMMENT:
      if (! reportComment(parser, parser->m_encoding, s, next))
        return XML_ERROR_NO_MEMORY;
      break;
    case XML_TOK_INVALID:
      parser->m_eventPtr = next;
      return XML_ERROR_INVALID_TOKEN;
    case XML_TOK_PARTIAL:
      if (! parser->m_parsingStatus.finalBuffer) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      return XML_ERROR_UNCLOSED_TOKEN;
    case XML_TOK_PARTIAL_CHAR:
      if (! parser->m_parsingStatus.finalBuffer) {
        *nextPtr = s;
        return XML_ERROR_NONE;
      }
      return XML_ERROR_PARTIAL_CHAR;
    default:
      return XML_ERROR_JUNK_AFTER_DOC_ELEMENT;
    }
    switch (parser->m_parsingStatus.parsing) {
    case XML_SUSPENDED:
      parser->m_eventPtr = next;
      *nextPtr = next;
      return XML_ERROR_NONE;
    case XML_FINISHED:
      parser->m_eventPtr = next;
      return XML_ERROR_ABORTED;
    case XML_PARSING:
      if (parser->m_reenter) {
        return XML_ERROR_UNEXPECTED_STATE; // LCOV_EXCL_LINE
      }
      EXPAT_FALLTHROUGH;
    default:;
      parser->m_eventPtr = s = next;
    }
  }
}

static enum XML_Error
processEntity(XML_Parser parser, ENTITY *entity, XML_Bool betweenDecl,
              enum EntityType type) {
  OPEN_INTERNAL_ENTITY *openEntity, **openEntityList;
  OPEN_INTERNAL_ENTITY **const freeEntityList = &parser->m_freeEntities;
  switch (type) {
  case ENTITY_INTERNAL:
    parser->m_processor = internalEntityProcessor;
    openEntityList = &parser->m_openInternalEntities;
    break;
  case ENTITY_ATTRIBUTE:
    openEntityList = &parser->m_openAttributeEntities;
    break;
  case ENTITY_VALUE:
    openEntityList = &parser->m_openValueEntities;
    break;
    /* default case serves merely as a safety net in case of a
     * wrong entityType. Therefore we exclude the following lines
     * from the test coverage.
     *
     * LCOV_EXCL_START
     */
  default:
    assert(0);
    /* LCOV_EXCL_STOP */
  }

  if (*freeEntityList) {
    openEntity = *freeEntityList;
    *freeEntityList = openEntity->next;
  } else {
    openEntity = MALLOC(parser, sizeof(OPEN_INTERNAL_ENTITY));
    if (! openEntity)
      return XML_ERROR_NO_MEMORY;
  }
  entity->open = XML_TRUE;
  entity->hasMore = XML_TRUE;
#if XML_GE == 1
  entityTrackingOnOpen(parser, entity, __LINE__);
#endif
  entity->processed = 0;
  openEntity->next = *openEntityList;
  *openEntityList = openEntity;
  openEntity->entity = entity;
  openEntity->type = type;
  openEntity->startTagLevel = parser->m_tagLevel;
  openEntity->betweenDecl = betweenDecl;
  openEntity->internalEventPtr = NULL;
  openEntity->internalEventEndPtr = NULL;

  if (type == ENTITY_INTERNAL) {
    triggerReenter(parser);
  }
  return XML_ERROR_NONE;
}

static enum XML_Error PTRCALL
internalEntityProcessor(XML_Parser parser, const char *s, const char *end,
                        const char **nextPtr) {
  UNUSED_P(s);
  UNUSED_P(end);
  UNUSED_P(nextPtr);
  ENTITY *entity;
  const char *textStart, *textEnd;
  const char *next;
  enum XML_Error result;
  OPEN_INTERNAL_ENTITY *openEntity = parser->m_openInternalEntities;
  if (! openEntity)
    return XML_ERROR_UNEXPECTED_STATE;

  entity = openEntity->entity;

  if (entity->hasMore) {
    textStart = ((const char *)entity->textPtr) + entity->processed;
    textEnd = (const char *)(entity->textPtr + entity->textLen);
    next = textStart;

    if (entity->is_param) {
      int tok
          = XmlPrologTok(parser->m_internalEncoding, textStart, textEnd, &next);
      result = doProlog(parser, parser->m_internalEncoding, textStart, textEnd,
                        tok, next, &next, XML_FALSE, XML_FALSE,
                        XML_ACCOUNT_ENTITY_EXPANSION);
    } else {
      result = doContent(parser, openEntity->startTagLevel,
                         parser->m_internalEncoding, textStart, textEnd, &next,
                         XML_FALSE, XML_ACCOUNT_ENTITY_EXPANSION);
    }

    if (result != XML_ERROR_NONE)
      return result;
    if (textEnd != next
        && (parser->m_parsingStatus.parsing == XML_SUSPENDED
            || (parser->m_parsingStatus.parsing == XML_PARSING
                && parser->m_reenter))) {
      entity->processed = (int)(next - (const char *)entity->textPtr);
      return result;
    }

    entity->hasMore = XML_FALSE;
    if (! entity->is_param
        && (openEntity->startTagLevel != parser->m_tagLevel)) {
      return XML_ERROR_ASYNC_ENTITY;
    }
    triggerReenter(parser);
    return result;
  } 

#if XML_GE == 1
  entityTrackingOnClose(parser, entity, __LINE__);
#endif
  assert(parser->m_openInternalEntities == openEntity);
  entity->open = XML_FALSE;
  parser->m_openInternalEntities = parser->m_openInternalEntities->next;

  openEntity->next = parser->m_freeEntities;
  parser->m_freeEntities = openEntity;

  if (parser->m_openInternalEntities == NULL) {
    parser->m_processor = entity->is_param ? prologProcessor : contentProcessor;
  }
  triggerReenter(parser);
  return XML_ERROR_NONE;
}

static enum XML_Error PTRCALL
errorProcessor(XML_Parser parser, const char *s, const char *end,
               const char **nextPtr) {
  UNUSED_P(s);
  UNUSED_P(end);
  UNUSED_P(nextPtr);
  return parser->m_errorCode;
}

static enum XML_Error
storeAttributeValue(XML_Parser parser, const ENCODING *enc, XML_Bool isCdata,
                    const char *ptr, const char *end, STRING_POOL *pool,
                    enum XML_Account account) {
  const char *next = ptr;
  enum XML_Error result = XML_ERROR_NONE;

  while (1) {
    if (! parser->m_openAttributeEntities) {
      result = appendAttributeValue(parser, enc, isCdata, next, end, pool,
                                    account, &next);
    } else {
      OPEN_INTERNAL_ENTITY *const openEntity = parser->m_openAttributeEntities;
      if (! openEntity)
        return XML_ERROR_UNEXPECTED_STATE;

      ENTITY *const entity = openEntity->entity;
      const char *const textStart
          = ((const char *)entity->textPtr) + entity->processed;
      const char *const textEnd
          = (const char *)(entity->textPtr + entity->textLen);
      const char *nextInEntity = textStart;
      if (entity->hasMore) {
        result = appendAttributeValue(
            parser, parser->m_internalEncoding, isCdata, textStart, textEnd,
            pool, XML_ACCOUNT_ENTITY_EXPANSION, &nextInEntity);
        if (result != XML_ERROR_NONE)
          break;
        if (textEnd != nextInEntity) {
          entity->processed
              = (int)(nextInEntity - (const char *)entity->textPtr);
          continue;
        }

        entity->hasMore = XML_FALSE;
        continue;
      } 

#if XML_GE == 1
      entityTrackingOnClose(parser, entity, __LINE__);
#endif
      assert(parser->m_openAttributeEntities == openEntity);
      entity->open = XML_FALSE;
      parser->m_openAttributeEntities = parser->m_openAttributeEntities->next;

      openEntity->next = parser->m_freeEntities;
      parser->m_freeEntities = openEntity;
    }

    if (result || (parser->m_openAttributeEntities == NULL && end == next)) {
      break;
    }
  }

  if (result)
    return result;
  if (! isCdata && poolLength(pool) && poolLastChar(pool) == 0x20)
    poolChop(pool);
  if (! poolAppendChar(pool, XML_T('\0')))
    return XML_ERROR_NO_MEMORY;
  return XML_ERROR_NONE;
}

static enum XML_Error
appendAttributeValue(XML_Parser parser, const ENCODING *enc, XML_Bool isCdata,
                     const char *ptr, const char *end, STRING_POOL *pool,
                     enum XML_Account account, const char **nextPtr) {
  DTD *const dtd = parser->m_dtd; 
#if !defined(XML_DTD)
  UNUSED_P(account);
#endif

  for (;;) {
    const char *next
        = ptr; 
    int tok = XmlAttributeValueTok(enc, ptr, end, &next);
#if XML_GE == 1
    if (! accountingDiffTolerated(parser, tok, ptr, next, __LINE__, account)) {
      accountingOnAbort(parser);
      return XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
    }
#endif
    switch (tok) {
    case XML_TOK_NONE:
      if (nextPtr) {
        *nextPtr = next;
      }
      return XML_ERROR_NONE;
    case XML_TOK_INVALID:
      if (enc == parser->m_encoding)
        parser->m_eventPtr = next;
      return XML_ERROR_INVALID_TOKEN;
    case XML_TOK_PARTIAL:
      if (enc == parser->m_encoding)
        parser->m_eventPtr = ptr;
      return XML_ERROR_INVALID_TOKEN;
    case XML_TOK_CHAR_REF: {
      XML_Char buf[XML_ENCODE_MAX];
      int n = XmlCharRefNumber(enc, ptr);
      if (n < 0) {
        if (enc == parser->m_encoding)
          parser->m_eventPtr = ptr;
        return XML_ERROR_BAD_CHAR_REF;
      }
      if (! isCdata && n == 0x20 
          && (poolLength(pool) == 0 || poolLastChar(pool) == 0x20))
        break;
      n = XmlEncode(n, (ICHAR *)buf);

      if (! poolAppendChars(pool, buf, n))
        return XML_ERROR_NO_MEMORY;
    } break;
    case XML_TOK_DATA_CHARS:
      if (! poolAppend(pool, enc, ptr, next))
        return XML_ERROR_NO_MEMORY;
      break;
    case XML_TOK_TRAILING_CR:
      next = ptr + enc->minBytesPerChar;
      EXPAT_FALLTHROUGH;
    case XML_TOK_ATTRIBUTE_VALUE_S:
    case XML_TOK_DATA_NEWLINE:
      if (! isCdata && (poolLength(pool) == 0 || poolLastChar(pool) == 0x20))
        break;
      if (! poolAppendChar(pool, 0x20))
        return XML_ERROR_NO_MEMORY;
      break;
    case XML_TOK_ENTITY_REF: {
      const XML_Char *name;
      ENTITY *entity;
      bool checkEntityDecl;
      XML_Char ch = (XML_Char)XmlPredefinedEntityName(
          enc, ptr + enc->minBytesPerChar, next - enc->minBytesPerChar);
      if (ch) {
#if XML_GE == 1
        accountingDiffTolerated(parser, tok, (char *)&ch,
                                ((char *)&ch) + sizeof(XML_Char), __LINE__,
                                XML_ACCOUNT_ENTITY_EXPANSION);
#endif
        if (! poolAppendChar(pool, ch))
          return XML_ERROR_NO_MEMORY;
        break;
      }
      name = poolStoreString(&parser->m_temp2Pool, enc,
                             ptr + enc->minBytesPerChar,
                             next - enc->minBytesPerChar);
      if (! name)
        return XML_ERROR_NO_MEMORY;
      entity = (ENTITY *)lookup(parser, &dtd->generalEntities, name, 0);
      poolDiscard(&parser->m_temp2Pool);
      if (pool == &dtd->pool) 
        checkEntityDecl =
#if defined(XML_DTD)
            parser->m_prologState.documentEntity &&
#endif
            (dtd->standalone ? ! parser->m_openInternalEntities
                             : ! dtd->hasParamEntityRefs);
      else 
        checkEntityDecl = ! dtd->hasParamEntityRefs || dtd->standalone;
      if (checkEntityDecl) {
        if (! entity)
          return XML_ERROR_UNDEFINED_ENTITY;
        else if (! entity->is_internal)
          return XML_ERROR_ENTITY_DECLARED_IN_PE;
      } else if (! entity) {
        return XML_ERROR_UNDEFINED_ENTITY;
      }
      if (entity->open) {
        if (enc == parser->m_encoding) {
          parser->m_eventPtr = ptr; /* LCOV_EXCL_LINE */
        }
        return XML_ERROR_RECURSIVE_ENTITY_REF;
      }
      if (entity->notation) {
        if (enc == parser->m_encoding)
          parser->m_eventPtr = ptr;
        return XML_ERROR_BINARY_ENTITY_REF;
      }
      if (! entity->textPtr) {
        if (enc == parser->m_encoding)
          parser->m_eventPtr = ptr;
        return XML_ERROR_ATTRIBUTE_EXTERNAL_ENTITY_REF;
      } else {
        enum XML_Error result;
        result = processEntity(parser, entity, XML_FALSE, ENTITY_ATTRIBUTE);
        if ((result == XML_ERROR_NONE) && (nextPtr != NULL)) {
          *nextPtr = next;
        }
        return result;
      }
    } break;
    default:
      /* The only token returned by XmlAttributeValueTok() that does
       * not have an explicit case here is XML_TOK_PARTIAL_CHAR.
       * Getting that would require an entity name to contain an
       * incomplete XML character (e.g. \xE2\x82); however previous
       * tokenisers will have already recognised and rejected such
       * names before XmlAttributeValueTok() gets a look-in.  This
       * default case should be retained as a safety net, but the code
       * excluded from coverage tests.
       *
       * LCOV_EXCL_START
       */
      if (enc == parser->m_encoding)
        parser->m_eventPtr = ptr;
      return XML_ERROR_UNEXPECTED_STATE;
      /* LCOV_EXCL_STOP */
    }
    ptr = next;
  }
}

#if XML_GE == 1
static enum XML_Error
storeEntityValue(XML_Parser parser, const ENCODING *enc,
                 const char *entityTextPtr, const char *entityTextEnd,
                 enum XML_Account account, const char **nextPtr) {
  DTD *const dtd = parser->m_dtd; 
  STRING_POOL *pool = &(dtd->entityValuePool);
  enum XML_Error result = XML_ERROR_NONE;
#if defined(XML_DTD)
  int oldInEntityValue = parser->m_prologState.inEntityValue;
  parser->m_prologState.inEntityValue = 1;
#else
  UNUSED_P(account);
#endif
  if (! pool->blocks) {
    if (! poolGrow(pool))
      return XML_ERROR_NO_MEMORY;
  }

  const char *next = entityTextPtr;

  if (entityTextPtr >= entityTextEnd) {
    result = XML_ERROR_NONE;
    goto endEntityValue;
  }

  for (;;) {
    next
        = entityTextPtr; 
    int tok = XmlEntityValueTok(enc, entityTextPtr, entityTextEnd, &next);

    if (! accountingDiffTolerated(parser, tok, entityTextPtr, next, __LINE__,
                                  account)) {
      accountingOnAbort(parser);
      result = XML_ERROR_AMPLIFICATION_LIMIT_BREACH;
      goto endEntityValue;
    }

    switch (tok) {
    case XML_TOK_PARAM_ENTITY_REF:
#if defined(XML_DTD)
      if (parser->m_isParamEntity || enc != parser->m_encoding) {
        const XML_Char *name;
        ENTITY *entity;
        name = poolStoreString(&parser->m_tempPool, enc,
                               entityTextPtr + enc->minBytesPerChar,
                               next - enc->minBytesPerChar);
        if (! name) {
          result = XML_ERROR_NO_MEMORY;
          goto endEntityValue;
        }
        entity = (ENTITY *)lookup(parser, &dtd->paramEntities, name, 0);
        poolDiscard(&parser->m_tempPool);
        if (! entity) {
          dtd->keepProcessing = dtd->standalone;
          goto endEntityValue;
        }
        if (entity->open || (entity == parser->m_declEntity)) {
          if (enc == parser->m_encoding)
            parser->m_eventPtr = entityTextPtr;
          result = XML_ERROR_RECURSIVE_ENTITY_REF;
          goto endEntityValue;
        }
        if (entity->systemId) {
          if (parser->m_externalEntityRefHandler) {
            dtd->paramEntityRead = XML_FALSE;
            entity->open = XML_TRUE;
            entityTrackingOnOpen(parser, entity, __LINE__);
            beforeHandler(parser);
            const int status = parser->m_externalEntityRefHandler(
                parser->m_externalEntityRefHandlerArg, 0, entity->base,
                entity->systemId, entity->publicId);
            afterHandler(parser);
            if (! status) {
              entityTrackingOnClose(parser, entity, __LINE__);
              entity->open = XML_FALSE;
              result = XML_ERROR_EXTERNAL_ENTITY_HANDLING;
              goto endEntityValue;
            }
            entityTrackingOnClose(parser, entity, __LINE__);
            entity->open = XML_FALSE;
            if (! dtd->paramEntityRead)
              dtd->keepProcessing = dtd->standalone;
          } else
            dtd->keepProcessing = dtd->standalone;
        } else {
          result = processEntity(parser, entity, XML_FALSE, ENTITY_VALUE);
          goto endEntityValue;
        }
        break;
      }
#endif
      parser->m_eventPtr = entityTextPtr;
      result = XML_ERROR_PARAM_ENTITY_REF;
      goto endEntityValue;
    case XML_TOK_NONE:
      result = XML_ERROR_NONE;
      goto endEntityValue;
    case XML_TOK_ENTITY_REF:
    case XML_TOK_DATA_CHARS:
      if (! poolAppend(pool, enc, entityTextPtr, next)) {
        result = XML_ERROR_NO_MEMORY;
        goto endEntityValue;
      }
      break;
    case XML_TOK_TRAILING_CR:
      next = entityTextPtr + enc->minBytesPerChar;
      EXPAT_FALLTHROUGH;
    case XML_TOK_DATA_NEWLINE:
      if (! poolAppendChar(pool, 0xA)) {
        result = XML_ERROR_NO_MEMORY;
        goto endEntityValue;
      }
      break;
    case XML_TOK_CHAR_REF: {
      XML_Char buf[XML_ENCODE_MAX];
      int n = XmlCharRefNumber(enc, entityTextPtr);
      if (n < 0) {
        if (enc == parser->m_encoding)
          parser->m_eventPtr = entityTextPtr;
        result = XML_ERROR_BAD_CHAR_REF;
        goto endEntityValue;
      }
      n = XmlEncode(n, (ICHAR *)buf);
      if (! poolAppendChars(pool, buf, n)) {
        result = XML_ERROR_NO_MEMORY;
        goto endEntityValue;
      }
    } break;
    case XML_TOK_PARTIAL:
      if (enc == parser->m_encoding)
        parser->m_eventPtr = entityTextPtr;
      result = XML_ERROR_INVALID_TOKEN;
      goto endEntityValue;
    case XML_TOK_INVALID:
      if (enc == parser->m_encoding)
        parser->m_eventPtr = next;
      result = XML_ERROR_INVALID_TOKEN;
      goto endEntityValue;
    default:
      /* This default case should be unnecessary -- all the tokens
       * that XmlEntityValueTok() can return have their own explicit
       * cases -- but should be retained for safety.  We do however
       * exclude it from the coverage statistics.
       *
       * LCOV_EXCL_START
       */
      if (enc == parser->m_encoding)
        parser->m_eventPtr = entityTextPtr;
      result = XML_ERROR_UNEXPECTED_STATE;
      goto endEntityValue;
      /* LCOV_EXCL_STOP */
    }
    entityTextPtr = next;
  }
endEntityValue:
#if defined(XML_DTD)
  parser->m_prologState.inEntityValue = oldInEntityValue;
#endif
  if (nextPtr != NULL) {
    *nextPtr = next;
  }
  return result;
}

static enum XML_Error
callStoreEntityValue(XML_Parser parser, const ENCODING *enc,
                     const char *entityTextPtr, const char *entityTextEnd,
                     enum XML_Account account) {
  const char *next = entityTextPtr;
  enum XML_Error result = XML_ERROR_NONE;
  while (1) {
    if (! parser->m_openValueEntities) {
      result
          = storeEntityValue(parser, enc, next, entityTextEnd, account, &next);
    } else {
      OPEN_INTERNAL_ENTITY *const openEntity = parser->m_openValueEntities;
      if (! openEntity)
        return XML_ERROR_UNEXPECTED_STATE;

      ENTITY *const entity = openEntity->entity;
      const char *const textStart
          = ((const char *)entity->textPtr) + entity->processed;
      const char *const textEnd
          = (const char *)(entity->textPtr + entity->textLen);
      const char *nextInEntity = textStart;
      if (entity->hasMore) {
        result = storeEntityValue(parser, parser->m_internalEncoding, textStart,
                                  textEnd, XML_ACCOUNT_ENTITY_EXPANSION,
                                  &nextInEntity);
        if (result != XML_ERROR_NONE)
          break;
        if (textEnd != nextInEntity) {
          entity->processed
              = (int)(nextInEntity - (const char *)entity->textPtr);
          continue;
        }

        entity->hasMore = XML_FALSE;
        continue;
      } 

#if XML_GE == 1
      entityTrackingOnClose(parser, entity, __LINE__);
#endif
      assert(parser->m_openValueEntities == openEntity);
      entity->open = XML_FALSE;
      parser->m_openValueEntities = parser->m_openValueEntities->next;

      openEntity->next = parser->m_freeEntities;
      parser->m_freeEntities = openEntity;
    }

    if (result
        || (parser->m_openValueEntities == NULL && entityTextEnd == next)) {
      break;
    }
  }

  return result;
}

#else

static enum XML_Error
storeSelfEntityValue(XML_Parser parser, ENTITY *entity) {
  const char *const entity_start = "&amp;";
  const char *const entity_end = ";";

  STRING_POOL *const pool = &(parser->m_dtd->entityValuePool);
  if (! poolAppendString(pool, entity_start)
      || ! poolAppendString(pool, entity->name)
      || ! poolAppendString(pool, entity_end)) {
    poolDiscard(pool);
    return XML_ERROR_NO_MEMORY;
  }

  if ((size_t)poolLength(pool) > (size_t)INT_MAX) {
    poolDiscard(pool);
    return XML_ERROR_NO_MEMORY;
  }
  entity->textPtr = poolStart(pool);
  entity->textLen = (int)(poolLength(pool));
  poolFinish(pool);

  return XML_ERROR_NONE;
}

#endif

static void FASTCALL
normalizeLines(XML_Char *s) {
  XML_Char *p;
  for (;; s++) {
    if (*s == XML_T('\0'))
      return;
    if (*s == 0xD)
      break;
  }
  p = s;
  do {
    if (*s == 0xD) {
      *p++ = 0xA;
      if (*++s == 0xA)
        s++;
    } else
      *p++ = *s++;
  } while (*s);
  *p = XML_T('\0');
}

static int
reportProcessingInstruction(XML_Parser parser, const ENCODING *enc,
                            const char *start, const char *end) {
  const XML_Char *target;
  XML_Char *data;
  const char *tem;
  if (! parser->m_processingInstructionHandler) {
    if (parser->m_defaultHandler)
      reportDefault(parser, enc, start, end);
    return 1;
  }
  start += enc->minBytesPerChar * 2;
  tem = start + XmlNameLength(enc, start);
  target = poolStoreString(&parser->m_tempPool, enc, start, tem);
  if (! target)
    return 0;
  poolFinish(&parser->m_tempPool);
  data = poolStoreString(&parser->m_tempPool, enc, XmlSkipS(enc, tem),
                         end - enc->minBytesPerChar * 2);
  if (! data)
    return 0;
  normalizeLines(data);
  beforeHandler(parser);
  parser->m_processingInstructionHandler(parser->m_handlerArg, target, data);
  afterHandler(parser);
  poolClear(&parser->m_tempPool);
  return 1;
}

static int
reportComment(XML_Parser parser, const ENCODING *enc, const char *start,
              const char *end) {
  XML_Char *data;
  if (! parser->m_commentHandler) {
    if (parser->m_defaultHandler)
      reportDefault(parser, enc, start, end);
    return 1;
  }
  data = poolStoreString(&parser->m_tempPool, enc,
                         start + enc->minBytesPerChar * 4,
                         end - enc->minBytesPerChar * 3);
  if (! data)
    return 0;
  normalizeLines(data);
  beforeHandler(parser);
  parser->m_commentHandler(parser->m_handlerArg, data);
  afterHandler(parser);
  poolClear(&parser->m_tempPool);
  return 1;
}

static void
reportDefault(XML_Parser parser, const ENCODING *enc, const char *s,
              const char *end) {
  if (MUST_CONVERT(enc, s)) {
    enum XML_Convert_Result convert_res;
    const char **eventPP;
    const char **eventEndPP;
    if (enc == parser->m_encoding) {
      eventPP = &parser->m_eventPtr;
      eventEndPP = &parser->m_eventEndPtr;
    } else {
      /* To get here, two things must be true; the parser must be
       * using a character encoding that is not the same as the
       * encoding passed in, and the encoding passed in must need
       * conversion to the internal format (UTF-8 unless XML_UNICODE
       * is defined).  The only occasions on which the encoding passed
       * in is not the same as the parser's encoding are when it is
       * the internal encoding (e.g. a previously defined parameter
       * entity, already converted to internal format).  This by
       * definition doesn't need conversion, so the whole branch never
       * gets executed.
       *
       * For safety's sake we don't delete these lines and merely
       * exclude them from coverage statistics.
       *
       * LCOV_EXCL_START
       */
      eventPP = &(parser->m_openInternalEntities->internalEventPtr);
      eventEndPP = &(parser->m_openInternalEntities->internalEventEndPtr);
      /* LCOV_EXCL_STOP */
    }
    do {
      ICHAR *dataPtr = (ICHAR *)parser->m_dataBuf;
      convert_res
          = XmlConvert(enc, &s, end, &dataPtr, (ICHAR *)parser->m_dataBufEnd);
      *eventEndPP = s;
      beforeHandler(parser);
      parser->m_defaultHandler(parser->m_handlerArg, parser->m_dataBuf,
                               (int)(dataPtr - (ICHAR *)parser->m_dataBuf));
      afterHandler(parser);
      *eventPP = s;
    } while ((convert_res != XML_CONVERT_COMPLETED)
             && (convert_res != XML_CONVERT_INPUT_INCOMPLETE));
  } else {
    beforeHandler(parser);
    parser->m_defaultHandler(
        parser->m_handlerArg, (const XML_Char *)s,
        (int)((const XML_Char *)end - (const XML_Char *)s));
    afterHandler(parser);
  }
}

static int
defineAttribute(ELEMENT_TYPE *type, ATTRIBUTE_ID *attId, XML_Bool isCdata,
                XML_Bool isId, const XML_Char *value, XML_Parser parser) {
  DEFAULT_ATTRIBUTE *att;
  if (value || isId) {
    NAMED *const nameFound
        = lookup(parser, &(type->defaultAttsNames), attId->name, 0);
    if (nameFound)
      return 1;
    if (isId && ! type->idAtt && ! attId->xmlns)
      type->idAtt = attId;
  }
  if (type->nDefaultAtts == type->allocDefaultAtts) {
    if (type->allocDefaultAtts > SIZE_MAX / 2) {
      return 0;
    }

    size_t count = type->allocDefaultAtts * 2;
    if (count == 0) {
      count = 8;
    }

    if (count > SIZE_MAX / sizeof(DEFAULT_ATTRIBUTE)) {
      return 0;
    }

    DEFAULT_ATTRIBUTE *const temp = REALLOC(
        parser, type->defaultAtts, (count * sizeof(DEFAULT_ATTRIBUTE)));
    if (temp == NULL)
      return 0;
    type->allocDefaultAtts = count;
    type->defaultAtts = temp;
  }
  att = type->defaultAtts + type->nDefaultAtts;
  att->id = attId;
  att->value = value;
  att->isCdata = isCdata;
  if (! isCdata)
    attId->maybeTokenized = XML_TRUE;

  NAMED *const nameAddedOrFound
      = lookup(parser, &(type->defaultAttsNames), attId->name, sizeof(NAMED));
  if (! nameAddedOrFound)
    return 0;

  type->nDefaultAtts += 1;
  return 1;
}

static int
setElementTypePrefix(XML_Parser parser, ELEMENT_TYPE *elementType) {
  DTD *const dtd = parser->m_dtd; 
  const XML_Char *name;
  for (name = elementType->name; *name; name++) {
    if (*name == XML_T(ASCII_COLON)) {
      PREFIX *prefix;
      const XML_Char *s;
      for (s = elementType->name; s != name; s++) {
        if (! poolAppendChar(&dtd->pool, *s))
          return 0;
      }
      if (! poolAppendChar(&dtd->pool, XML_T('\0')))
        return 0;
      prefix = (PREFIX *)lookup(parser, &dtd->prefixes, poolStart(&dtd->pool),
                                sizeof(PREFIX));
      if (! prefix)
        return 0;
      if (prefix->name == poolStart(&dtd->pool))
        poolFinish(&dtd->pool);
      else
        poolDiscard(&dtd->pool);
      elementType->prefix = prefix;
      break;
    }
  }
  return 1;
}

static ATTRIBUTE_ID *
getAttributeId(XML_Parser parser, const ENCODING *enc, const char *start,
               const char *end) {
  DTD *const dtd = parser->m_dtd; 
  ATTRIBUTE_ID *id;
  const XML_Char *name;
  if (! poolAppendChar(&dtd->pool, XML_T('\0')))
    return NULL;
  name = poolStoreString(&dtd->pool, enc, start, end);
  if (! name)
    return NULL;
  ++name;
  id = (ATTRIBUTE_ID *)lookup(parser, &dtd->attributeIds, name,
                              sizeof(ATTRIBUTE_ID));
  if (! id)
    return NULL;
  if (id->name != name)
    poolDiscard(&dtd->pool);
  else {
    poolFinish(&dtd->pool);
    if (! parser->m_ns)
      ;
    else if (name[0] == XML_T(ASCII_x) && name[1] == XML_T(ASCII_m)
             && name[2] == XML_T(ASCII_l) && name[3] == XML_T(ASCII_n)
             && name[4] == XML_T(ASCII_s)
             && (name[5] == XML_T('\0') || name[5] == XML_T(ASCII_COLON))) {
      if (name[5] == XML_T('\0'))
        id->prefix = &dtd->defaultPrefix;
      else
        id->prefix = (PREFIX *)lookup(parser, &dtd->prefixes, name + 6,
                                      sizeof(PREFIX));
      id->xmlns = XML_TRUE;
    } else {
      int i;
      for (i = 0; name[i]; i++) {
        if (i == INT_MAX) {
          return NULL;
        }
        if (name[i] == XML_T(ASCII_COLON)) {
          if (! poolAppendChars(&dtd->pool, name, i))
            return NULL;
          if (! poolAppendChar(&dtd->pool, XML_T('\0')))
            return NULL;
          id->prefix = (PREFIX *)lookup(parser, &dtd->prefixes,
                                        poolStart(&dtd->pool), sizeof(PREFIX));
          if (! id->prefix)
            return NULL;
          if (id->prefix->name == poolStart(&dtd->pool))
            poolFinish(&dtd->pool);
          else
            poolDiscard(&dtd->pool);
          break;
        }
      }
    }
  }
  return id;
}

#define CONTEXT_SEP XML_T(ASCII_FF)

static const XML_Char *
getContext(XML_Parser parser) {
  DTD *const dtd = parser->m_dtd; 
  HASH_TABLE_ITER iter;
  XML_Bool needSep = XML_FALSE;

  if (dtd->defaultPrefix.binding) {
    if (! poolAppendChar(&parser->m_tempPool, XML_T(ASCII_EQUALS)))
      return NULL;
    size_t len = dtd->defaultPrefix.binding->uriLen;
    if (parser->m_namespaceSeparator)
      len--;
    if (! poolAppendChars(&parser->m_tempPool, dtd->defaultPrefix.binding->uri,
                          len)) {
      return NULL; /* LCOV_EXCL_LINE */
    }
    needSep = XML_TRUE;
  }

  hashTableIterInit(&iter, &(dtd->prefixes));
  for (;;) {
    PREFIX *prefix = (PREFIX *)hashTableIterNext(&iter);
    if (! prefix)
      break;
    if (! prefix->binding) {
      continue; /* LCOV_EXCL_LINE */
    }
    if (needSep && ! poolAppendChar(&parser->m_tempPool, CONTEXT_SEP))
      return NULL;
    if (! poolAppendChars(&parser->m_tempPool, prefix->name,
                          xcslen(prefix->name)))
      return NULL;
    if (! poolAppendChar(&parser->m_tempPool, XML_T(ASCII_EQUALS)))
      return NULL;
    size_t len = prefix->binding->uriLen;
    if (parser->m_namespaceSeparator)
      len--;
    if (! poolAppendChars(&parser->m_tempPool, prefix->binding->uri, len))
      return NULL;
    needSep = XML_TRUE;
  }

  hashTableIterInit(&iter, &(dtd->generalEntities));
  for (;;) {
    ENTITY *e = (ENTITY *)hashTableIterNext(&iter);
    if (! e)
      break;
    if (! e->open)
      continue;
    if (needSep && ! poolAppendChar(&parser->m_tempPool, CONTEXT_SEP))
      return NULL;
    if (! poolAppendChars(&parser->m_tempPool, e->name, xcslen(e->name)))
      return NULL;
    needSep = XML_TRUE;
  }

  if (! poolAppendChar(&parser->m_tempPool, XML_T('\0')))
    return NULL;
  return parser->m_tempPool.start;
}

static XML_Bool
setContext(XML_Parser parser, const XML_Char *context) {
  if (context == NULL) {
    return XML_FALSE;
  }

  DTD *const dtd = parser->m_dtd; 
  const XML_Char *s = context;

  while (*context != XML_T('\0')) {
    if (*s == CONTEXT_SEP || *s == XML_T('\0')) {
      ENTITY *e;
      if (! poolAppendChar(&parser->m_tempPool, XML_T('\0')))
        return XML_FALSE;
      e = (ENTITY *)lookup(parser, &dtd->generalEntities,
                           poolStart(&parser->m_tempPool), 0);
      if (e)
        e->open = XML_TRUE;
      if (*s != XML_T('\0'))
        s++;
      context = s;
      poolDiscard(&parser->m_tempPool);
    } else if (*s == XML_T(ASCII_EQUALS)) {
      PREFIX *prefix;
      if (poolLength(&parser->m_tempPool) == 0)
        prefix = &dtd->defaultPrefix;
      else {
        if (! poolAppendChar(&parser->m_tempPool, XML_T('\0')))
          return XML_FALSE;
        const XML_Char *const prefixName = poolCopyStringNoFinish(
            &dtd->pool, poolStart(&parser->m_tempPool));
        if (! prefixName) {
          return XML_FALSE;
        }

        prefix = (PREFIX *)lookup(parser, &dtd->prefixes, prefixName,
                                  sizeof(PREFIX));

        const bool prefixNameUsed = prefix && prefix->name == prefixName;
        if (prefixNameUsed)
          poolFinish(&dtd->pool);
        else
          poolDiscard(&dtd->pool);

        if (! prefix)
          return XML_FALSE;

        poolDiscard(&parser->m_tempPool);
      }
      for (context = s + 1; *context != CONTEXT_SEP && *context != XML_T('\0');
           context++)
        if (! poolAppendChar(&parser->m_tempPool, *context))
          return XML_FALSE;
      if (! poolAppendChar(&parser->m_tempPool, XML_T('\0')))
        return XML_FALSE;
      if (addBinding(parser, prefix, NULL, poolStart(&parser->m_tempPool),
                     &parser->m_inheritedBindings)
          != XML_ERROR_NONE)
        return XML_FALSE;
      poolDiscard(&parser->m_tempPool);
      if (*context != XML_T('\0'))
        ++context;
      s = context;
    } else {
      if (! poolAppendChar(&parser->m_tempPool, *s))
        return XML_FALSE;
      s++;
    }
  }
  return XML_TRUE;
}

static void FASTCALL
normalizePublicId(XML_Char *publicId) {
  XML_Char *p = publicId;
  XML_Char *s;
  for (s = publicId; *s; s++) {
    switch (*s) {
    case 0x20:
    case 0xD:
    case 0xA:
      if (p != publicId && p[-1] != 0x20)
        *p++ = 0x20;
      break;
    default:
      *p++ = *s;
    }
  }
  if (p != publicId && p[-1] == 0x20)
    --p;
  *p = XML_T('\0');
}

static DTD *
dtdCreate(XML_Parser parser) {
  DTD *p = MALLOC(parser, sizeof(DTD));
  if (p == NULL)
    return p;
  poolInit(&(p->pool), parser);
  poolInit(&(p->entityValuePool), parser);
  hashTableInit(&(p->generalEntities), parser);
  hashTableInit(&(p->elementTypes), parser);
  hashTableInit(&(p->attributeIds), parser);
  hashTableInit(&(p->prefixes), parser);
#if defined(XML_DTD)
  p->paramEntityRead = XML_FALSE;
  hashTableInit(&(p->paramEntities), parser);
#endif
  p->defaultPrefix.name = NULL;
  p->defaultPrefix.binding = NULL;

  p->in_eldecl = XML_FALSE;
  p->scaffIndex = NULL;
  p->scaffIndexSize = 0;
  p->scaffold = NULL;
  p->scaffLevel = 0;
  p->scaffSize = 0;
  p->scaffCount = 0;
  p->contentStringLen = 0;

  p->keepProcessing = XML_TRUE;
  p->hasParamEntityRefs = XML_FALSE;
  p->standalone = XML_FALSE;
  return p;
}


static void
dtdDestroy(DTD *p, XML_Bool isDocEntity, XML_Parser parser) {
  HASH_TABLE_ITER iter;
  hashTableIterInit(&iter, &(p->elementTypes));
  for (;;) {
    ELEMENT_TYPE *e = (ELEMENT_TYPE *)hashTableIterNext(&iter);
    if (! e)
      break;
    hashTableDestroy(&(e->defaultAttsNames));
    FREE(parser, e->defaultAtts);
  }
  hashTableDestroy(&(p->generalEntities));
#if defined(XML_DTD)
  hashTableDestroy(&(p->paramEntities));
#endif
  hashTableDestroy(&(p->elementTypes));
  hashTableDestroy(&(p->attributeIds));
  hashTableDestroy(&(p->prefixes));
  poolDestroy(&(p->pool));
  poolDestroy(&(p->entityValuePool));
  if (isDocEntity) {
    FREE(parser, p->scaffIndex);
    FREE(parser, p->scaffold);
  }
  FREE(parser, p);
}

static int
dtdCopy(XML_Parser oldParser, DTD *newDtd, const DTD *oldDtd,
        XML_Parser parser) {
  HASH_TABLE_ITER iter;


  hashTableIterInit(&iter, &(oldDtd->prefixes));
  for (;;) {
    const XML_Char *name;
    const PREFIX *oldP = (PREFIX *)hashTableIterNext(&iter);
    if (! oldP)
      break;
    name = poolCopyString(&(newDtd->pool), oldP->name);
    if (! name)
      return 0;
    if (! lookup(oldParser, &(newDtd->prefixes), name, sizeof(PREFIX)))
      return 0;
  }

  hashTableIterInit(&iter, &(oldDtd->attributeIds));


  for (;;) {
    ATTRIBUTE_ID *newA;
    const XML_Char *name;
    const ATTRIBUTE_ID *oldA = (ATTRIBUTE_ID *)hashTableIterNext(&iter);

    if (! oldA)
      break;
    if (! poolAppendChar(&(newDtd->pool), XML_T('\0')))
      return 0;
    name = poolCopyString(&(newDtd->pool), oldA->name);
    if (! name)
      return 0;
    ++name;
    newA = (ATTRIBUTE_ID *)lookup(oldParser, &(newDtd->attributeIds), name,
                                  sizeof(ATTRIBUTE_ID));
    if (! newA)
      return 0;
    newA->maybeTokenized = oldA->maybeTokenized;
    if (oldA->prefix) {
      newA->xmlns = oldA->xmlns;
      if (oldA->prefix == &oldDtd->defaultPrefix)
        newA->prefix = &newDtd->defaultPrefix;
      else
        newA->prefix = (PREFIX *)lookup(oldParser, &(newDtd->prefixes),
                                        oldA->prefix->name, 0);
    }
  }


  hashTableIterInit(&iter, &(oldDtd->elementTypes));

  for (;;) {
    ELEMENT_TYPE *newE;
    const XML_Char *name;
    const ELEMENT_TYPE *oldE = (ELEMENT_TYPE *)hashTableIterNext(&iter);
    if (! oldE)
      break;
    name = poolCopyString(&(newDtd->pool), oldE->name);
    if (! name)
      return 0;
    newE = (ELEMENT_TYPE *)lookup(oldParser, &(newDtd->elementTypes), name,
                                  sizeof(ELEMENT_TYPE));
    if (! newE)
      return 0;

    if (! newE->defaultAttsNames.parser)
      hashTableInit(&(newE->defaultAttsNames), parser);

    if (oldE->nDefaultAtts) {
      if (oldE->nDefaultAtts > SIZE_MAX / sizeof(DEFAULT_ATTRIBUTE)) {
        return 0;
      }
      newE->defaultAtts
          = MALLOC(parser, oldE->nDefaultAtts * sizeof(DEFAULT_ATTRIBUTE));
      if (! newE->defaultAtts) {
        return 0;
      }
    }
    if (oldE->idAtt)
      newE->idAtt = (ATTRIBUTE_ID *)lookup(oldParser, &(newDtd->attributeIds),
                                           oldE->idAtt->name, 0);
    newE->allocDefaultAtts = newE->nDefaultAtts = oldE->nDefaultAtts;
    if (oldE->prefix)
      newE->prefix = (PREFIX *)lookup(oldParser, &(newDtd->prefixes),
                                      oldE->prefix->name, 0);
    for (size_t i = 0; i < newE->nDefaultAtts; i++) {
      const XML_Char *const attributeName = oldE->defaultAtts[i].id->name;
      newE->defaultAtts[i].id = (ATTRIBUTE_ID *)lookup(
          oldParser, &(newDtd->attributeIds), attributeName, 0);
      newE->defaultAtts[i].isCdata = oldE->defaultAtts[i].isCdata;
      if (oldE->defaultAtts[i].value) {
        newE->defaultAtts[i].value
            = poolCopyString(&(newDtd->pool), oldE->defaultAtts[i].value);
        if (! newE->defaultAtts[i].value)
          return 0;
      } else
        newE->defaultAtts[i].value = NULL;

      NAMED *const nameAddedOrFound = lookup(parser, &(newE->defaultAttsNames),
                                             attributeName, sizeof(NAMED));
      if (! nameAddedOrFound) {
        return 0;
      }
    }
  }

  if (! copyEntityTable(oldParser, &(newDtd->generalEntities), &(newDtd->pool),
                        &(oldDtd->generalEntities)))
    return 0;

#if defined(XML_DTD)
  if (! copyEntityTable(oldParser, &(newDtd->paramEntities), &(newDtd->pool),
                        &(oldDtd->paramEntities)))
    return 0;
  newDtd->paramEntityRead = oldDtd->paramEntityRead;
#endif

  newDtd->keepProcessing = oldDtd->keepProcessing;
  newDtd->hasParamEntityRefs = oldDtd->hasParamEntityRefs;
  newDtd->standalone = oldDtd->standalone;

  newDtd->in_eldecl = oldDtd->in_eldecl;
  newDtd->scaffold = oldDtd->scaffold;
  newDtd->contentStringLen = oldDtd->contentStringLen;
  newDtd->scaffSize = oldDtd->scaffSize;
  newDtd->scaffLevel = oldDtd->scaffLevel;
  newDtd->scaffIndex = oldDtd->scaffIndex;
  newDtd->scaffIndexSize = oldDtd->scaffIndexSize;

  return 1;
} 

static int
copyEntityTable(XML_Parser oldParser, HASH_TABLE *newTable,
                STRING_POOL *newPool, const HASH_TABLE *oldTable) {
  HASH_TABLE_ITER iter;
  const XML_Char *cachedOldBase = NULL;
  const XML_Char *cachedNewBase = NULL;

  hashTableIterInit(&iter, oldTable);

  for (;;) {
    ENTITY *newE;
    const XML_Char *name;
    const ENTITY *oldE = (ENTITY *)hashTableIterNext(&iter);
    if (! oldE)
      break;
    name = poolCopyString(newPool, oldE->name);
    if (! name)
      return 0;
    newE = (ENTITY *)lookup(oldParser, newTable, name, sizeof(ENTITY));
    if (! newE)
      return 0;
    if (oldE->systemId) {
      const XML_Char *tem = poolCopyString(newPool, oldE->systemId);
      if (! tem)
        return 0;
      newE->systemId = tem;
      if (oldE->base) {
        if (oldE->base == cachedOldBase)
          newE->base = cachedNewBase;
        else {
          cachedOldBase = oldE->base;
          tem = poolCopyString(newPool, cachedOldBase);
          if (! tem)
            return 0;
          cachedNewBase = newE->base = tem;
        }
      }
      if (oldE->publicId) {
        tem = poolCopyString(newPool, oldE->publicId);
        if (! tem)
          return 0;
        newE->publicId = tem;
      }
    } else {
      const XML_Char *tem
          = poolCopyStringN(newPool, oldE->textPtr, oldE->textLen);
      if (! tem)
        return 0;
      newE->textPtr = tem;
      newE->textLen = oldE->textLen;
    }
    if (oldE->notation) {
      const XML_Char *tem = poolCopyString(newPool, oldE->notation);
      if (! tem)
        return 0;
      newE->notation = tem;
    }
    newE->is_param = oldE->is_param;
    newE->is_internal = oldE->is_internal;
  }
  return 1;
}

#define INIT_POWER 6

static XML_Bool FASTCALL
keyeq(KEY s1, KEY s2) {
#if defined(XML_UNICODE)
#if defined(XML_UNICODE_WCHAR_T)
  return (wcscmp(s1, s2) == 0) ? XML_TRUE : XML_FALSE;
#else
  for (; *s1 == *s2; s1++, s2++)
    if (*s1 == 0)
      return XML_TRUE;
  return XML_FALSE;
#endif
#else
  return (strcmp(s1, s2) == 0) ? XML_TRUE : XML_FALSE;
#endif
}

static size_t
keylen(KEY s) {
  return xcslen(s);
}

static void
copy_salt_to_sipkey(XML_Parser parser, struct sipkey *key) {
  const XML_Parser rootParser = getRootParserOf(parser, NULL);
  assert(! rootParser->m_parentParser);

  *key = rootParser->m_hash_secret_salt_128;
}

static unsigned long FASTCALL
hash(XML_Parser parser, KEY s) {
  struct siphash state;
  struct sipkey key;
  (void)sip24_valid;
  copy_salt_to_sipkey(parser, &key);
  sip24_init(&state, &key);
  sip24_update(&state, s, keylen(s) * sizeof(XML_Char));
  return (unsigned long)sip24_final(&state);
}

static NAMED *
lookup(XML_Parser parser, HASH_TABLE *table, KEY name, size_t createSize) {
  size_t i;
  if (table->size == 0) {
    size_t tsize;
    if (! createSize)
      return NULL;
    table->power = INIT_POWER;
    table->size = (size_t)1 << INIT_POWER;
    tsize = table->size * sizeof(NAMED *);
    table->v = MALLOC(table->parser, tsize);
    if (! table->v) {
      table->size = 0;
      return NULL;
    }
    memset(table->v, 0, tsize);
    i = hash(parser, name) & ((unsigned long)table->size - 1);
  } else {
    unsigned long h = hash(parser, name);
    unsigned long mask = (unsigned long)table->size - 1;
    unsigned char step = 0;
    i = h & mask;
    while (table->v[i]) {
      if (keyeq(name, table->v[i]->name))
        return table->v[i];
      if (! step)
        step = PROBE_STEP(h, mask, table->power);
      i < step ? (i += table->size - step) : (i -= step);
    }
    if (! createSize)
      return NULL;

    if (table->used >> (table->power - 1)) {
      unsigned char newPower = table->power + 1;

      if (newPower >= sizeof(unsigned long) * 8 ) {
        return NULL;
      }

      size_t newSize = (size_t)1 << newPower;
      unsigned long newMask = (unsigned long)newSize - 1;

      if (newSize > SIZE_MAX / sizeof(NAMED *)) {
        return NULL;
      }

      size_t tsize = newSize * sizeof(NAMED *);
      NAMED **newV = MALLOC(table->parser, tsize);
      if (! newV)
        return NULL;
      memset(newV, 0, tsize);
      for (i = 0; i < table->size; i++)
        if (table->v[i]) {
          unsigned long newHash = hash(parser, table->v[i]->name);
          size_t j = newHash & newMask;
          step = 0;
          while (newV[j]) {
            if (! step)
              step = PROBE_STEP(newHash, newMask, newPower);
            j < step ? (j += newSize - step) : (j -= step);
          }
          newV[j] = table->v[i];
        }
      FREE(table->parser, table->v);
      table->v = newV;
      table->power = newPower;
      table->size = newSize;
      i = h & newMask;
      step = 0;
      while (table->v[i]) {
        if (! step)
          step = PROBE_STEP(h, newMask, newPower);
        i < step ? (i += newSize - step) : (i -= step);
      }
    }
  }
  table->v[i] = MALLOC(table->parser, createSize);
  if (! table->v[i])
    return NULL;
  memset(table->v[i], 0, createSize);
  table->v[i]->name = name;
  (table->used)++;
  return table->v[i];
}


static void FASTCALL
hashTableDestroy(HASH_TABLE *table) {
  size_t i;
  for (i = 0; i < table->size; i++)
    FREE(table->parser, table->v[i]);
  FREE(table->parser, table->v);
}

static void FASTCALL
hashTableInit(HASH_TABLE *p, XML_Parser parser) {
  p->power = 0;
  p->size = 0;
  p->used = 0;
  p->v = NULL;
  p->parser = parser;
}

static void FASTCALL
hashTableIterInit(HASH_TABLE_ITER *iter, const HASH_TABLE *table) {
  iter->p = table->v;
  iter->end = iter->p ? iter->p + table->size : NULL;
}

static NAMED *FASTCALL
hashTableIterNext(HASH_TABLE_ITER *iter) {
  while (iter->p != iter->end) {
    NAMED *tem = *(iter->p)++;
    if (tem)
      return tem;
  }
  return NULL;
}

static void FASTCALL
poolInit(STRING_POOL *pool, XML_Parser parser) {
  pool->blocks = NULL;
  pool->freeBlocks = NULL;
  pool->start = NULL;
  pool->ptr = NULL;
  pool->end = NULL;
  pool->parser = parser;
}

static void FASTCALL
poolClear(STRING_POOL *pool) {
  if (! pool->freeBlocks)
    pool->freeBlocks = pool->blocks;
  else {
    BLOCK *p = pool->blocks;
    while (p) {
      BLOCK *tem = p->next;
      p->next = pool->freeBlocks;
      pool->freeBlocks = p;
      p = tem;
    }
  }
  pool->blocks = NULL;
  pool->start = NULL;
  pool->ptr = NULL;
  pool->end = NULL;
}

static void FASTCALL
poolDestroy(STRING_POOL *pool) {
  BLOCK *p = pool->blocks;
  while (p) {
    BLOCK *tem = p->next;
    FREE(pool->parser, p);
    p = tem;
  }
  p = pool->freeBlocks;
  while (p) {
    BLOCK *tem = p->next;
    FREE(pool->parser, p);
    p = tem;
  }
}

static XML_Char *
poolAppend(STRING_POOL *pool, const ENCODING *enc, const char *ptr,
           const char *end) {
  if (! pool->ptr && ! poolGrow(pool))
    return NULL;
  for (;;) {
    const enum XML_Convert_Result convert_res = XmlConvert(
        enc, &ptr, end, (ICHAR **)&(pool->ptr), (const ICHAR *)pool->end);
    if ((convert_res == XML_CONVERT_COMPLETED)
        || (convert_res == XML_CONVERT_INPUT_INCOMPLETE))
      break;
    if (! poolGrow(pool))
      return NULL;
  }
  return pool->start;
}

static const XML_Char *FASTCALL
poolCopyString(STRING_POOL *pool, const XML_Char *s) {
  if (! poolAppendChars(pool, s, xcslen(s) +  1))
    return NULL;
  s = pool->start;
  poolFinish(pool);
  return s;
}

static const XML_Char *FASTCALL
poolCopyStringNoFinish(STRING_POOL *pool, const XML_Char *s) {
  const XML_Char *const original = s;
  do {
    if (! poolAppendChar(pool, *s)) {
      const ptrdiff_t advancedBy = s - original;
      if (advancedBy > 0)
        pool->ptr -= advancedBy;
      return NULL;
    }
  } while (*s++);
  return pool->start;
}

static const XML_Char *
poolCopyStringN(STRING_POOL *pool, const XML_Char *s, int n) {
  if (! pool->ptr && ! poolGrow(pool)) {
    return NULL; /* LCOV_EXCL_LINE */
  }
  if (n > 0 && ! poolAppendChars(pool, s, n))
    return NULL;
  s = pool->start;
  poolFinish(pool);
  return s;
}

static const XML_Char *FASTCALL
poolAppendString(STRING_POOL *pool, const XML_Char *s) {
  if (! poolAppendChars(pool, s, xcslen(s)))
    return NULL;
  return pool->start;
}

static XML_Char *
poolStoreString(STRING_POOL *pool, const ENCODING *enc, const char *ptr,
                const char *end) {
  if (! poolAppend(pool, enc, ptr, end))
    return NULL;
  if (! poolAppendChar(pool, 0))
    return NULL;
  return pool->start;
}

static size_t
poolBytesToAllocateFor(int blockSize) {
  const size_t stretch = sizeof(XML_Char); 

  if (blockSize <= 0)
    return 0;

  if (blockSize > (int)(INT_MAX / stretch))
    return 0;

  {
    const int stretchedBlockSize = blockSize * (int)stretch;
    const int bytesToAllocate
        = (int)(offsetof(BLOCK, s) + (unsigned)stretchedBlockSize);
    if (bytesToAllocate < 0)
      return 0;

    return (size_t)bytesToAllocate;
  }
}

static XML_Bool FASTCALL
poolGrow(STRING_POOL *pool) {
  if (pool->freeBlocks) {
    if (pool->start == NULL) {
      pool->blocks = pool->freeBlocks;
      pool->freeBlocks = pool->freeBlocks->next;
      pool->blocks->next = NULL;
      pool->start = pool->blocks->s;
      pool->end = pool->start + pool->blocks->size;
      pool->ptr = pool->start;
      return XML_TRUE;
    }
    if (pool->end - pool->start < pool->freeBlocks->size) {
      BLOCK *tem = pool->freeBlocks->next;
      pool->freeBlocks->next = pool->blocks;
      pool->blocks = pool->freeBlocks;
      pool->freeBlocks = tem;
      memcpy(pool->blocks->s, pool->start,
             (pool->end - pool->start) * sizeof(XML_Char));
      pool->ptr = pool->blocks->s + (pool->ptr - pool->start);
      pool->start = pool->blocks->s;
      pool->end = pool->start + pool->blocks->size;
      return XML_TRUE;
    }
  }
  if (pool->blocks && pool->start == pool->blocks->s) {
    BLOCK *temp;
    int blockSize = (int)((unsigned)(pool->end - pool->start) * 2U);
    size_t bytesToAllocate;

    const ptrdiff_t offsetInsideBlock = pool->ptr - pool->start;

    if (blockSize < 0) {
      return XML_FALSE; /* LCOV_EXCL_LINE */
    }

    bytesToAllocate = poolBytesToAllocateFor(blockSize);
    if (bytesToAllocate == 0)
      return XML_FALSE;

    temp = REALLOC(pool->parser, pool->blocks, bytesToAllocate);
    if (temp == NULL)
      return XML_FALSE;
    pool->blocks = temp;
    pool->blocks->size = blockSize;
    pool->ptr = pool->blocks->s + offsetInsideBlock;
    pool->start = pool->blocks->s;
    pool->end = pool->start + blockSize;
  } else {
    BLOCK *tem;
    int blockSize = (int)(pool->end - pool->start);
    size_t bytesToAllocate;

    if (blockSize < 0) {
      return XML_FALSE; /* LCOV_EXCL_LINE */
    }

    if (blockSize < INIT_BLOCK_SIZE)
      blockSize = INIT_BLOCK_SIZE;
    else {
      if ((int)((unsigned)blockSize * 2U) < 0) {
        return XML_FALSE;
      }
      blockSize *= 2;
    }

    bytesToAllocate = poolBytesToAllocateFor(blockSize);
    if (bytesToAllocate == 0)
      return XML_FALSE;

    tem = MALLOC(pool->parser, bytesToAllocate);
    if (! tem)
      return XML_FALSE;
    tem->size = blockSize;
    tem->next = pool->blocks;
    pool->blocks = tem;
    if (pool->ptr != pool->start)
      memcpy(tem->s, pool->start, (pool->ptr - pool->start) * sizeof(XML_Char));
    pool->ptr = tem->s + (pool->ptr - pool->start);
    pool->start = tem->s;
    pool->end = tem->s + blockSize;
  }
  return XML_TRUE;
}

static bool FASTCALL
poolGrowUntil(STRING_POOL *pool, size_t needed) {
  for (;;) {
    const size_t available = pool->end - pool->ptr;
    if (available >= needed) {
      return true;
    }
    if (! poolGrow(pool)) {
      return false;
    }
  }
}

static int FASTCALL
nextScaffoldPart(XML_Parser parser) {
  DTD *const dtd = parser->m_dtd; 
  CONTENT_SCAFFOLD *me;
  int next;

  if (! dtd->scaffIndex) {
    if (parser->m_groupSize > SIZE_MAX / sizeof(int)) {
      return -1;
    }
    dtd->scaffIndex = MALLOC(parser, parser->m_groupSize * sizeof(int));
    if (! dtd->scaffIndex)
      return -1;
    dtd->scaffIndexSize = parser->m_groupSize;
    dtd->scaffIndex[0] = 0;
  }

  if (dtd->scaffCount > INT_MAX) {
    return -1;
  }

  if (dtd->scaffCount >= dtd->scaffSize) {
    CONTENT_SCAFFOLD *temp;
    if (dtd->scaffold) {
      if (dtd->scaffSize > UINT_MAX / 2u) {
        return -1;
      }
#if UINT_MAX >= SIZE_MAX
      if (dtd->scaffSize > SIZE_MAX / 2u / sizeof(CONTENT_SCAFFOLD)) {
        return -1;
      }
#endif

      temp = REALLOC(parser, dtd->scaffold,
                     dtd->scaffSize * 2 * sizeof(CONTENT_SCAFFOLD));
      if (temp == NULL)
        return -1;
      dtd->scaffSize *= 2;
    } else {
      temp = MALLOC(parser, INIT_SCAFFOLD_ELEMENTS * sizeof(CONTENT_SCAFFOLD));
      if (temp == NULL)
        return -1;
      dtd->scaffSize = INIT_SCAFFOLD_ELEMENTS;
    }
    dtd->scaffold = temp;
  }
  next = (int)dtd->scaffCount++;
  me = &dtd->scaffold[next];
  if (dtd->scaffLevel) {
    CONTENT_SCAFFOLD *parent
        = &dtd->scaffold[dtd->scaffIndex[dtd->scaffLevel - 1]];
    if (parent->lastchild) {
      dtd->scaffold[parent->lastchild].nextsib = next;
    }
    if (! parent->childcnt)
      parent->firstchild = next;
    parent->lastchild = next;
    parent->childcnt++;
  }
  me->firstchild = me->lastchild = me->childcnt = me->nextsib = 0;
  return next;
}

static XML_Content *
build_model(XML_Parser parser) {
  DTD *const dtd = parser->m_dtd; 
  XML_Content *ret;
  XML_Char *str; 

#if UINT_MAX >= SIZE_MAX
  if (dtd->scaffCount > SIZE_MAX / sizeof(XML_Content)) {
    return NULL;
  }
  if (dtd->contentStringLen > SIZE_MAX / sizeof(XML_Char)) {
    return NULL;
  }
#endif
  if (dtd->scaffCount * sizeof(XML_Content)
      > SIZE_MAX - dtd->contentStringLen * sizeof(XML_Char)) {
    return NULL;
  }

  const size_t allocsize = (dtd->scaffCount * sizeof(XML_Content)
                            + (dtd->contentStringLen * sizeof(XML_Char)));

  ret = parser->m_mem.malloc_fcn(allocsize);
  if (! ret)
    return NULL;

  XML_Content *dest = ret; 
  XML_Content *const destLimit = &ret[dtd->scaffCount];
  XML_Content *jobDest = ret; 
  str = (XML_Char *)&ret[dtd->scaffCount];

  (jobDest++)->numchildren = 0;

  for (; dest < destLimit; dest++) {
    const int src_node = (int)dest->numchildren;

    dest->type = dtd->scaffold[src_node].type;
    dest->quant = dtd->scaffold[src_node].quant;
    if (dest->type == XML_CTYPE_NAME) {
      const XML_Char *src;
      dest->name = str;
      src = dtd->scaffold[src_node].name;

      const size_t nameLen = xcslen(src) +  1;

      if (nameLen > SIZE_MAX / sizeof(XML_Char)) {
        parser->m_mem.free_fcn(ret);
        return NULL;
      }

      memcpy(str, src, nameLen * sizeof(XML_Char));
      str += nameLen;

      dest->numchildren = 0;
      dest->children = NULL;
    } else {
      unsigned int i;
      int cn;
      dest->name = NULL;
      dest->numchildren = dtd->scaffold[src_node].childcnt;
      dest->children = jobDest;

      for (i = 0, cn = dtd->scaffold[src_node].firstchild;
           i < dest->numchildren; i++, cn = dtd->scaffold[cn].nextsib)
        (jobDest++)->numchildren = (unsigned int)cn;
    }
  }

  return ret;
}

static ELEMENT_TYPE *
getElementType(XML_Parser parser, const ENCODING *enc, const char *ptr,
               const char *end) {
  DTD *const dtd = parser->m_dtd; 
  const XML_Char *name = poolStoreString(&dtd->pool, enc, ptr, end);
  ELEMENT_TYPE *ret;

  if (! name)
    return NULL;
  ret = (ELEMENT_TYPE *)lookup(parser, &dtd->elementTypes, name,
                               sizeof(ELEMENT_TYPE));
  if (! ret)
    return NULL;
  if (! ret->defaultAttsNames.parser)
    hashTableInit(&(ret->defaultAttsNames), getRootParserOf(parser, NULL));
  if (ret->name != name)
    poolDiscard(&dtd->pool);
  else {
    poolFinish(&dtd->pool);
    if (! setElementTypePrefix(parser, ret))
      return NULL;
  }
  return ret;
}

static XML_Char *
copyString(const XML_Char *s, XML_Parser parser) {
  const size_t charsRequired = xcslen(s) +  1;

  if (charsRequired > SIZE_MAX / sizeof(XML_Char))
    return NULL;

  const size_t bytesRequired = charsRequired * sizeof(XML_Char);

  XML_Char *const result = MALLOC(parser, bytesRequired);

  if (result == NULL)
    return NULL;

  memcpy(result, s, bytesRequired);

  return result;
}

#if XML_GE == 1

static float
accountingGetCurrentAmplification(XML_Parser rootParser) {
  const size_t lenOfShortestInclude = sizeof("<!ENTITY a SYSTEM 'b'>") - 1;
  const XmlBigCount countBytesOutput
      = rootParser->m_accounting.countBytesDirect
        + rootParser->m_accounting.countBytesIndirect;
  const float amplificationFactor
      = rootParser->m_accounting.countBytesDirect
            ? ((float)countBytesOutput
               / (float)(rootParser->m_accounting.countBytesDirect))
            : ((float)(lenOfShortestInclude
                       + rootParser->m_accounting.countBytesIndirect)
               / (float)lenOfShortestInclude);
  assert(! rootParser->m_parentParser);
  return amplificationFactor;
}

static void
accountingReportStats(XML_Parser originParser, const char *epilog) {
}

static void
accountingOnAbort(XML_Parser originParser) {
  accountingReportStats(originParser, " ABORTING\n");
}


static XML_Bool
accountingDiffTolerated(XML_Parser originParser, int tok, const char *before,
                        const char *after, int source_line,
                        enum XML_Account account) {
  switch (tok) {
  case XML_TOK_INVALID:
  case XML_TOK_PARTIAL:
  case XML_TOK_PARTIAL_CHAR:
  case XML_TOK_NONE:
    return XML_TRUE;
  }

  if (account == XML_ACCOUNT_NONE)
    return XML_TRUE; 

  unsigned int levelsAwayFromRootParser;
  const XML_Parser rootParser
      = getRootParserOf(originParser, &levelsAwayFromRootParser);
  assert(! rootParser->m_parentParser);

  const int isDirect
      = (account == XML_ACCOUNT_DIRECT) && (originParser == rootParser);
  const ptrdiff_t bytesMore = after - before;

  XmlBigCount *const additionTarget
      = isDirect ? &rootParser->m_accounting.countBytesDirect
                 : &rootParser->m_accounting.countBytesIndirect;

  if (*additionTarget > (XmlBigCount)(-1) - (XmlBigCount)bytesMore)
    return XML_FALSE;
  *additionTarget += bytesMore;

  const XmlBigCount countBytesOutput
      = rootParser->m_accounting.countBytesDirect
        + rootParser->m_accounting.countBytesIndirect;
  const float amplificationFactor
      = accountingGetCurrentAmplification(rootParser);
  const XML_Bool tolerated
      = (countBytesOutput < rootParser->m_accounting.activationThresholdBytes)
        || (amplificationFactor
            <= rootParser->m_accounting.maximumAmplificationFactor);


  return tolerated;
}



static void
entityTrackingOnOpen(XML_Parser originParser, ENTITY *entity, int sourceLine) {
}

static void
entityTrackingOnClose(XML_Parser originParser, ENTITY *entity, int sourceLine) {
}

#endif

static XML_Parser
getRootParserOf(XML_Parser parser, unsigned int *outLevelDiff) {
  XML_Parser rootParser = parser;
  unsigned int stepsTakenUpwards = 0;
  while (rootParser->m_parentParser) {
    rootParser = rootParser->m_parentParser;
    stepsTakenUpwards++;
  }
  assert(! rootParser->m_parentParser);
  if (outLevelDiff != NULL) {
    *outLevelDiff = stepsTakenUpwards;
  }
  return rootParser;
}

#if XML_GE == 1


#endif


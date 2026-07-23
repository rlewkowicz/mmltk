/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsExpatDriver.h"
#include "mozilla/fallible.h"
#include "nsCOMPtr.h"
#include "CParserContext.h"
#include "nsIExpatSink.h"
#include "nsIContentSink.h"
#include "nsIDocShell.h"
#include "nsParserMsgUtils.h"
#include "nsIURL.h"
#include "nsIUnicharInputStream.h"
#include "nsIProtocolHandler.h"
#include "nsNetUtil.h"
#include "nsString.h"
#include "nsTextFormatter.h"
#include "nsDirectoryServiceDefs.h"
#include "nsCRT.h"
#include "nsIConsoleService.h"
#include "nsIScriptError.h"
#include "nsIScriptGlobalObject.h"
#include "nsIContentPolicy.h"
#include "nsComponentManagerUtils.h"
#include "nsContentPolicyUtils.h"
#include "nsError.h"
#include "nsXPCOMCIDInternal.h"
#include "nsUnicharInputStream.h"
#include "nsContentUtils.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/NullPrincipal.h"
#include "mozilla/RandomNum.h"

#include "nsThreadUtils.h"
#include "mozilla/Logging.h"

using mozilla::fallible;
using mozilla::LogLevel;
using mozilla::MakeStringSpan;
using mozilla::Maybe;
using mozilla::dom::Document;

#ifdef DEBUG
static const uint32_t sMaxChunkLength = 1024 / sizeof(char16_t);
#else
static const uint32_t sMaxChunkLength = (128 * 1024) / sizeof(char16_t);
#endif

#define kExpatSeparatorChar 0xFFFF

static const char16_t kUTF16[] = {'U', 'T', 'F', '-', '1', '6', '\0'};

static mozilla::LazyLogModule gExpatDriverLog("expatdriver");

static const uint16_t sMaxXMLTreeDepth = 5000;


static void Driver_HandleXMLDeclaration(void* aUserData,
                                        const XML_Char* aVersion,
                                        const XML_Char* aEncoding,
                                        int aStandalone) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  MOZ_RELEASE_ASSERT(aStandalone >= -1 && aStandalone <= 1);
  driver->HandleXMLDeclaration(aVersion, aEncoding, aStandalone);
}

static void Driver_HandleCharacterData(void* aUserData,
                                       const XML_Char* aData, int aLength) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  MOZ_RELEASE_ASSERT(aLength >= 0);
  driver->HandleCharacterData(aData, static_cast<uint32_t>(aLength));
}

static void Driver_HandleComment(void* aUserData, const XML_Char* aName) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  driver->HandleComment(aName);
}

static void Driver_HandleProcessingInstruction(void* aUserData,
                                               const XML_Char* aTarget,
                                               const XML_Char* aData) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  driver->HandleProcessingInstruction(aTarget, aData);
}

static void Driver_HandleDefault(void* aUserData, const XML_Char* aData,
                                 int aLength) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  MOZ_RELEASE_ASSERT(aLength >= 0);
  driver->HandleDefault(aData, static_cast<uint32_t>(aLength));
}

static void Driver_HandleStartCdataSection(void* aUserData) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  driver->HandleStartCdataSection();
}

static void Driver_HandleEndCdataSection(void* aUserData) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  driver->HandleEndCdataSection();
}

static void Driver_HandleStartDoctypeDecl(void* aUserData,
                                          const XML_Char* aDoctypeName,
                                          const XML_Char* aSysid,
                                          const XML_Char* aPubid,
                                          int aHasInternalSubset) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  driver->HandleStartDoctypeDecl(aDoctypeName, aSysid, aPubid,
                                 !!aHasInternalSubset);
}

static void Driver_HandleEndDoctypeDecl(void* aUserData) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  driver->HandleEndDoctypeDecl();
}

static int Driver_HandleExternalEntityRef(void* aUserData,
                                          const XML_Char* aOpenEntityNames,
                                          const XML_Char* aBase,
                                          const XML_Char* aSystemId,
                                          const XML_Char* aPublicId) {
  auto* driver = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(driver);
  return driver->HandleExternalEntityRef(aOpenEntityNames, aBase, aSystemId,
                                         aPublicId);
}



struct nsCatalogData {
  const char* mPublicID;
  const char* mLocalDTD;
  const char* mAgentSheet;
};

static const nsCatalogData kCatalogTable[] = {
    {"-//W3C//DTD XHTML 1.0 Transitional//EN", "htmlmathml-f.ent", nullptr},
    {"-//W3C//DTD XHTML 1.1//EN", "htmlmathml-f.ent", nullptr},
    {"-//W3C//DTD XHTML 1.0 Strict//EN", "htmlmathml-f.ent", nullptr},
    {"-//W3C//DTD XHTML 1.0 Frameset//EN", "htmlmathml-f.ent", nullptr},
    {"-//W3C//DTD XHTML Basic 1.0//EN", "htmlmathml-f.ent", nullptr},
    {"-//W3C//DTD XHTML 1.1 plus MathML 2.0//EN", "htmlmathml-f.ent", nullptr},
    {"-//W3C//DTD XHTML 1.1 plus MathML 2.0 plus SVG 1.1//EN",
     "htmlmathml-f.ent", nullptr},
    {"-//W3C//DTD MathML 2.0//EN", "htmlmathml-f.ent", nullptr},
    {"-//WAPFORUM//DTD XHTML Mobile 1.0//EN", "htmlmathml-f.ent", nullptr},
    {"-//WAPFORUM//DTD XHTML Mobile 1.1//EN", "htmlmathml-f.ent", nullptr},
    {"-//WAPFORUM//DTD XHTML Mobile 1.2//EN", "htmlmathml-f.ent", nullptr},
    {nullptr, nullptr, nullptr}};

static const nsCatalogData* LookupCatalogData(const char16_t* aPublicID) {
  nsDependentString publicID(aPublicID);

  const nsCatalogData* data = kCatalogTable;
  while (data->mPublicID) {
    if (publicID.EqualsASCII(data->mPublicID)) {
      return data;
    }
    ++data;
  }

  return nullptr;
}

static void GetLocalDTDURI(const nsCatalogData* aCatalogData, nsIURI* aDTD,
                           nsIURI** aResult) {
  nsAutoCString fileName;
  if (aCatalogData) {
    fileName.Assign(aCatalogData->mLocalDTD);
  }

  if (fileName.IsEmpty()) {
    nsCOMPtr<nsIURL> dtdURL = do_QueryInterface(aDTD);
    if (!dtdURL) {
      return;
    }

    dtdURL->GetFileName(fileName);
    if (fileName.IsEmpty()) {
      return;
    }
  }

  nsAutoCString respath("resource://gre/res/dtd/");
  respath += fileName;
  NS_NewURI(aResult, respath);
}


NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(nsExpatDriver)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(nsExpatDriver)
NS_IMPL_CYCLE_COLLECTING_RELEASE(nsExpatDriver)

NS_IMPL_CYCLE_COLLECTION(nsExpatDriver, mSink)

nsExpatDriver::nsExpatDriver()
    : mExpatParser(nullptr),
      mInCData(false),
      mInInternalSubset(false),
      mInExternalDTD(false),
      mMadeFinalCallToExpat(false),
      mInParser(false),
      mInternalState(NS_OK),
      mExpatBuffered(0),
      mTagDepth(0),
      mCatalogData(nullptr),
      mInnerWindowID(0) {}

nsExpatDriver::~nsExpatDriver() { Destroy(); }

void nsExpatDriver::Destroy() {
  if (mExpatParser) {
    MOZ_XML_ParserFree(mExpatParser);
  }
  mURIs.Clear();
  mExpatParser = nullptr;
}

void nsExpatDriver::HandleStartElement(void* aUserData,
                                       const char16_t* aName,
                                       const char16_t** aAttrs) {
  auto* self = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(self && self->mSink);

  int count = MOZ_XML_GetSpecifiedAttributeCount(self->mExpatParser);
  MOZ_RELEASE_ASSERT(count >= 0, "Unexpected attribute count");
  MOZ_RELEASE_ASSERT(count % 2 == 0, "Attribute count must be even");

  uint32_t attrArrayLength = static_cast<uint32_t>(count);
  for (; aAttrs[attrArrayLength] != nullptr; attrArrayLength += 2) {
    MOZ_RELEASE_ASSERT(attrArrayLength <= UINT32_MAX - 2,
                       "Attribute count overflow");
  }

  if (self->mSink) {
    static_assert(
        sMaxXMLTreeDepth <=
        std::numeric_limits<decltype(nsExpatDriver::mTagDepth)>::max());

    if (++self->mTagDepth > sMaxXMLTreeDepth) {
      self->MaybeStopParser(NS_ERROR_HTMLPARSER_HIERARCHYTOODEEP);
      return;
    }

    nsresult rv = self->mSink->HandleStartElement(
        aName, aAttrs, attrArrayLength,
        MOZ_XML_GetCurrentLineNumber(self->mExpatParser),
        MOZ_XML_GetCurrentColumnNumber(self->mExpatParser));
    self->MaybeStopParser(rv);
  }
}

void nsExpatDriver::HandleStartElementForSystemPrincipal(
    void* aUserData, const char16_t* aName, const char16_t** aAttrs) {
  auto* self = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(self);
  if (!MOZ_XML_ProcessingEntityValue(self->mExpatParser)) {
    HandleStartElement(aUserData, aName, aAttrs);
  } else {
    nsCOMPtr<Document> doc =
        do_QueryInterface(self->mOriginalSink->GetTarget());

    XML_Size colNumber =
        MOZ_XML_GetCurrentColumnNumber(self->mExpatParser) + 1;
    XML_Size lineNumber = MOZ_XML_GetCurrentLineNumber(self->mExpatParser);

    int32_t nameSpaceID;
    RefPtr<nsAtom> prefix, localName;
    nsContentUtils::SplitExpatName(aName, getter_AddRefs(prefix),
                                   getter_AddRefs(localName), &nameSpaceID);

    nsAutoString error;
    error.AppendLiteral("Ignoring element <");
    if (prefix) {
      error.Append(prefix->GetUTF16String());
      error.Append(':');
    }
    error.Append(localName->GetUTF16String());
    error.AppendLiteral("> created from entity value.");

    nsContentUtils::ReportToConsoleNonLocalized(
        error, nsIScriptError::warningFlag, "XML Document"_ns, doc,
        mozilla::SourceLocation(doc->GetDocumentURI(), lineNumber, colNumber));
  }
}

void nsExpatDriver::HandleEndElement(void* aUserData,
                                     const char16_t* aName) {
  auto* self = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(self);

  NS_ASSERTION(self->mSink, "content sink not found!");
  NS_ASSERTION(self->mInternalState != NS_ERROR_HTMLPARSER_BLOCK,
               "Shouldn't block from HandleStartElement.");

  if (self->mSink && self->mInternalState != NS_ERROR_HTMLPARSER_STOPPARSING) {
    nsresult rv = self->mSink->HandleEndElement(aName);
    --self->mTagDepth;
    self->MaybeStopParser(rv);
  }
}

void nsExpatDriver::HandleEndElementForSystemPrincipal(
    void* aUserData, const char16_t* aName) {
  auto* self = static_cast<nsExpatDriver*>(aUserData);
  MOZ_ASSERT(self);
  if (!MOZ_XML_ProcessingEntityValue(self->mExpatParser)) {
    HandleEndElement(aUserData, aName);
  }
}

nsresult nsExpatDriver::HandleCharacterData(const char16_t* aValue,
                                            const uint32_t aLength) {
  NS_ASSERTION(mSink, "content sink not found!");

  if (mInCData) {
    if (!mCDataText.Append(aValue, aLength, fallible)) {
      MaybeStopParser(NS_ERROR_OUT_OF_MEMORY);
    }
  } else if (mSink) {
    nsresult rv = mSink->HandleCharacterData(aValue, aLength);
    MaybeStopParser(rv);
  }

  return NS_OK;
}

nsresult nsExpatDriver::HandleComment(const char16_t* aValue) {
  NS_ASSERTION(mSink, "content sink not found!");

  if (mInExternalDTD) {
    return NS_OK;
  }

  if (mInInternalSubset) {
    mInternalSubset.AppendLiteral("<!--");
    mInternalSubset.Append(aValue);
    mInternalSubset.AppendLiteral("-->");
  } else if (mSink) {
    nsresult rv = mSink->HandleComment(aValue);
    MaybeStopParser(rv);
  }

  return NS_OK;
}

nsresult nsExpatDriver::HandleProcessingInstruction(const char16_t* aTarget,
                                                    const char16_t* aData) {
  NS_ASSERTION(mSink, "content sink not found!");

  if (mInExternalDTD) {
    return NS_OK;
  }

  if (mInInternalSubset) {
    mInternalSubset.AppendLiteral("<?");
    mInternalSubset.Append(aTarget);
    mInternalSubset.Append(' ');
    mInternalSubset.Append(aData);
    mInternalSubset.AppendLiteral("?>");
  } else if (mSink) {
    nsresult rv = mSink->HandleProcessingInstruction(aTarget, aData);
    MaybeStopParser(rv);
  }

  return NS_OK;
}

nsresult nsExpatDriver::HandleXMLDeclaration(const char16_t* aVersion,
                                             const char16_t* aEncoding,
                                             int32_t aStandalone) {
  if (mSink) {
    nsresult rv = mSink->HandleXMLDeclaration(aVersion, aEncoding, aStandalone);
    MaybeStopParser(rv);
  }

  return NS_OK;
}

nsresult nsExpatDriver::HandleDefault(const char16_t* aValue,
                                      const uint32_t aLength) {
  NS_ASSERTION(mSink, "content sink not found!");

  if (mInExternalDTD) {
    return NS_OK;
  }

  if (mInInternalSubset) {
    mInternalSubset.Append(aValue, aLength);
  } else if (mSink) {
    uint32_t i;
    nsresult rv = mInternalState;
    for (i = 0; i < aLength && NS_SUCCEEDED(rv); ++i) {
      if (aValue[i] == '\n' || aValue[i] == '\r') {
        rv = mSink->HandleCharacterData(&aValue[i], 1);
      }
    }
    MaybeStopParser(rv);
  }

  return NS_OK;
}

nsresult nsExpatDriver::HandleStartCdataSection() {
  mInCData = true;

  return NS_OK;
}

nsresult nsExpatDriver::HandleEndCdataSection() {
  NS_ASSERTION(mSink, "content sink not found!");

  mInCData = false;
  if (mSink) {
    nsresult rv =
        mSink->HandleCDataSection(mCDataText.get(), mCDataText.Length());
    MaybeStopParser(rv);
  }
  mCDataText.Truncate();

  return NS_OK;
}

nsresult nsExpatDriver::HandleStartDoctypeDecl(const char16_t* aDoctypeName,
                                               const char16_t* aSysid,
                                               const char16_t* aPubid,
                                               bool aHasInternalSubset) {
  mDoctypeName = aDoctypeName;
  mSystemID = aSysid;
  mPublicID = aPubid;

  if (aHasInternalSubset) {
    mInInternalSubset = true;
    mInternalSubset.SetCapacity(1024);
  } else {
    mInternalSubset.SetIsVoid(true);
  }

  return NS_OK;
}

nsresult nsExpatDriver::HandleEndDoctypeDecl() {
  NS_ASSERTION(mSink, "content sink not found!");

  mInInternalSubset = false;

  if (mSink) {
    nsCOMPtr<nsIURI> data;
#if 0
    if (mCatalogData && mCatalogData->mAgentSheet) {
      NS_NewURI(getter_AddRefs(data), mCatalogData->mAgentSheet);
    }
#endif

    MOZ_ASSERT(!mCatalogData || !mCatalogData->mAgentSheet,
               "Need to add back support for catalog style sheets");

    nsresult rv = mSink->HandleDoctypeDecl(mInternalSubset, mDoctypeName,
                                           mSystemID, mPublicID, data);
    MaybeStopParser(rv);
  }

  mInternalSubset.Truncate();

  return NS_OK;
}

static nsresult ExternalDTDStreamReaderFunc(nsIUnicharInputStream* aIn,
                                            void* aClosure,
                                            const char16_t* aFromSegment,
                                            uint32_t aToOffset, uint32_t aCount,
                                            uint32_t* aWriteCount) {
  MOZ_ASSERT(aClosure && aFromSegment && aWriteCount);

  auto parser = static_cast<XML_Parser>(aClosure);
  if (MOZ_XML_Parse(parser, reinterpret_cast<const char*>(aFromSegment),
                    aCount * sizeof(char16_t), XML_FALSE) == XML_STATUS_OK) {
    *aWriteCount = aCount;
    return NS_OK;
  }

  *aWriteCount = 0;
  return NS_ERROR_FAILURE;
}

int nsExpatDriver::HandleExternalEntityRef(const char16_t* openEntityNames,
                                           const char16_t* base,
                                           const char16_t* systemId,
                                           const char16_t* publicId) {
  if (mInInternalSubset && !mInExternalDTD && openEntityNames) {
    mInternalSubset.Append(char16_t('%'));
    mInternalSubset.Append(nsDependentString(openEntityNames));
    mInternalSubset.Append(char16_t(';'));
  }

  nsCOMPtr<nsIURI> baseURI = GetBaseURI(base);
  NS_ENSURE_TRUE(baseURI, 1);

  nsCOMPtr<nsIInputStream> in;
  nsCOMPtr<nsIURI> absURI;
  nsresult rv = OpenInputStreamFromExternalDTD(
      publicId, systemId, baseURI, getter_AddRefs(in), getter_AddRefs(absURI));
  if (NS_FAILED(rv)) {
#ifdef DEBUG
    nsCString message("Failed to open external DTD: publicId \"");
    AppendUTF16toUTF8(MakeStringSpan(publicId), message);
    message += "\" systemId \"";
    AppendUTF16toUTF8(MakeStringSpan(systemId), message);
    message += "\" base \"";
    message.Append(baseURI->GetSpecOrDefault());
    message += "\" URL \"";
    if (absURI) {
      message.Append(absURI->GetSpecOrDefault());
    }
    message += "\"";
    NS_WARNING(message.get());
#endif
    return 1;
  }

  nsCOMPtr<nsIUnicharInputStream> uniIn;
  rv = NS_NewUnicharInputStream(in, getter_AddRefs(uniIn));
  NS_ENSURE_SUCCESS(rv, 1);

  int result = 1;
  if (uniIn) {
    XML_Parser entParser =
        MOZ_XML_ExternalEntityParserCreate(mExpatParser, nullptr, kUTF16);
    if (entParser) {
      auto baseURI = GetExpatBaseURI(absURI);
      MOZ_XML_SetBase(entParser, baseURI.begin());

      mInExternalDTD = true;

      bool inParser = mInParser;  
      mInParser = true;

      uint32_t totalRead;
      do {
        rv = uniIn->ReadSegments(ExternalDTDStreamReaderFunc, entParser,
                                 uint32_t(-1), &totalRead);
      } while (NS_SUCCEEDED(rv) && totalRead > 0);

      result = MOZ_XML_Parse(entParser, nullptr, 0, XML_TRUE);

      mInParser = inParser;  
      mInExternalDTD = false;

      MOZ_XML_ParserFree(entParser);
    }
  }

  return result;
}

nsresult nsExpatDriver::OpenInputStreamFromExternalDTD(const char16_t* aFPIStr,
                                                       const char16_t* aURLStr,
                                                       nsIURI* aBaseURI,
                                                       nsIInputStream** aStream,
                                                       nsIURI** aAbsURI) {
  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewURI(getter_AddRefs(uri), NS_ConvertUTF16toUTF8(aURLStr),
                          nullptr, aBaseURI);
  if (NS_WARN_IF(NS_FAILED(rv) && rv != NS_ERROR_MALFORMED_URI)) {
    return rv;
  }

  bool isUIResource = false;
  if (uri) {
    rv = NS_URIChainHasFlags(uri, nsIProtocolHandler::URI_IS_UI_RESOURCE,
                             &isUIResource);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsCOMPtr<nsIURI> localURI;
  if (!isUIResource) {
    if (aFPIStr) {
      mCatalogData = LookupCatalogData(aFPIStr);
      GetLocalDTDURI(mCatalogData, uri, getter_AddRefs(localURI));
    }
    if (!localURI) {
      return NS_ERROR_NOT_IMPLEMENTED;
    }
  }

  nsCOMPtr<nsIChannel> channel;
  if (localURI) {
    localURI.swap(uri);
    rv = NS_NewChannel(getter_AddRefs(channel), uri,
                       nsContentUtils::GetSystemPrincipal(),
                       nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
                       nsIContentPolicy::TYPE_DTD);
    NS_ENSURE_SUCCESS(rv, rv);
  } else {
    NS_ASSERTION(
        mSink == nsCOMPtr<nsIExpatSink>(do_QueryInterface(mOriginalSink)),
        "In nsExpatDriver::OpenInputStreamFromExternalDTD: "
        "mOriginalSink not the same object as mSink?");
    nsContentPolicyType policyType = nsIContentPolicy::TYPE_INTERNAL_DTD;
    if (mOriginalSink) {
      nsCOMPtr<Document> doc;
      doc = do_QueryInterface(mOriginalSink->GetTarget());
      if (doc) {
        if (doc->SkipDTDSecurityChecks()) {
          policyType = nsIContentPolicy::TYPE_INTERNAL_FORCE_ALLOWED_DTD;
        }
        rv = NS_NewChannel(
            getter_AddRefs(channel), uri, doc,
            nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_INHERITS_SEC_CONTEXT |
                nsILoadInfo::SEC_ALLOW_CHROME,
            policyType);
        NS_ENSURE_SUCCESS(rv, rv);
      }
    }
    if (!channel) {
      nsCOMPtr<nsIPrincipal> nullPrincipal =
          mozilla::NullPrincipal::CreateWithoutOriginAttributes();
      rv = NS_NewChannel(
          getter_AddRefs(channel), uri, nullPrincipal,
          nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_INHERITS_SEC_CONTEXT |
              nsILoadInfo::SEC_ALLOW_CHROME,
          policyType);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  uri.forget(aAbsURI);

  channel->SetContentType("application/xml"_ns);
  return channel->Open(aStream);
}

static nsresult CreateErrorText(const char16_t* aDescription,
                                const char16_t* aSourceURL,
                                XML_Size aLineNumber, XML_Size aColNumber,
                                nsString& aErrorString, bool spoofEnglish) {
  aErrorString.Truncate();

  nsAutoString msg;
  nsresult rv = nsParserMsgUtils::GetLocalizedStringByName(
      spoofEnglish ? XMLPARSER_PROPERTIES_en_US : XMLPARSER_PROPERTIES,
      "XMLParsingError", msg);
  NS_ENSURE_SUCCESS(rv, rv);

  nsTextFormatter::ssprintf(aErrorString, msg.get(), aDescription, aSourceURL,
                            aLineNumber, aColNumber);
  return NS_OK;
}

static nsresult AppendErrorPointer(XML_Size aColNumber,
                                   const char16_t* aSourceLine,
                                   size_t aSourceLineLength,
                                   nsString& aSourceString) {
  aSourceString.Append(char16_t('\n'));

  MOZ_RELEASE_ASSERT(aColNumber != static_cast<XML_Size>(0),
                     "Unexpected value of column");

  XML_Size last = aColNumber - 1;
  if (last > aSourceLineLength) {
    last = 0;
  }

  XML_Size i;
  uint32_t minuses = 0;
  for (i = 0; i < last; ++i) {
    if (aSourceLine[i] == '\t') {
      uint32_t add = 8 - (minuses % 8);
      aSourceString.AppendASCII("--------", add);
      minuses += add;
    } else {
      aSourceString.Append(char16_t('-'));
      ++minuses;
    }
  }
  aSourceString.Append(char16_t('^'));

  return NS_OK;
}

nsresult nsExpatDriver::HandleError() {
  int32_t code = MOZ_XML_GetErrorCode(mExpatParser);

  nsAutoString description;
  nsCOMPtr<Document> doc;
  if (mOriginalSink) {
    doc = do_QueryInterface(mOriginalSink->GetTarget());
  }

  bool spoofEnglish = nsContentUtils::ShouldResistFingerprinting(
      doc, mozilla::RFPTarget::JSLocale);
  nsParserMsgUtils::GetLocalizedStringByID(
      spoofEnglish ? XMLPARSER_PROPERTIES_en_US : XMLPARSER_PROPERTIES, code,
      description);

  if (code == XML_ERROR_TAG_MISMATCH) {

    const char16_t* mismatch = MOZ_XML_GetMismatchedTag(mExpatParser);
    const char16_t* uriEnd = nullptr;
    const char16_t* nameEnd = nullptr;
    const char16_t* pos;
    for (pos = mismatch; *pos; ++pos) {
      if (*pos == kExpatSeparatorChar) {
        if (uriEnd) {
          nameEnd = pos;
        } else {
          uriEnd = pos;
        }
      }
    }

    nsAutoString tagName;
    if (uriEnd && nameEnd) {
      tagName.Append(nameEnd + 1, pos - nameEnd - 1);
      tagName.Append(char16_t(':'));
    }
    const char16_t* nameStart = uriEnd ? uriEnd + 1 : mismatch;
    tagName.Append(nameStart, (nameEnd ? nameEnd : pos) - nameStart);

    nsAutoString msg;
    nsParserMsgUtils::GetLocalizedStringByName(
        spoofEnglish ? XMLPARSER_PROPERTIES_en_US : XMLPARSER_PROPERTIES,
        "Expected", msg);

    nsAutoString message;
    nsTextFormatter::ssprintf(message, msg.get(), tagName.get());
    description.Append(message);
  }

  XML_Size colNumber = MOZ_XML_GetCurrentColumnNumber(mExpatParser) + 1;
  XML_Size lineNumber = MOZ_XML_GetCurrentLineNumber(mExpatParser);

  const XML_Char* expatBase = MOZ_XML_GetBase(mExpatParser);
  nsAutoString uri;
  nsCOMPtr<nsIURI> baseURI;
  if (expatBase && (baseURI = GetBaseURI(expatBase))) {
    (void)CopyUTF8toUTF16(baseURI->GetSpecOrDefault(), uri, fallible);
  }
  nsAutoString errorText;
  CreateErrorText(description.get(), uri.get(), lineNumber, colNumber,
                  errorText, spoofEnglish);

  nsAutoString sourceText(mLastLine);
  AppendErrorPointer(colNumber, mLastLine.get(), mLastLine.Length(),
                     sourceText);

  if (doc && nsContentUtils::IsChromeDoc(doc)) {
    nsCString path = doc->GetDocumentURI()->GetSpecOrDefault();
    nsCOMPtr<nsISupports> container = doc->GetContainer();
    nsCOMPtr<nsIDocShell> docShell = do_QueryInterface(container);
    nsCString docShellDestroyed("unknown"_ns);
    if (docShell) {
      bool destroyed = false;
      docShell->IsBeingDestroyed(&destroyed);
      docShellDestroyed.Assign(destroyed ? "true"_ns : "false"_ns);
    }



  }

  nsCOMPtr<nsIScriptError> serr(do_CreateInstance(NS_SCRIPTERROR_CONTRACTID));
  nsresult rv = NS_ERROR_FAILURE;
  if (serr) {
    rv = serr->InitWithSourceURI(
        errorText, mURIs.SafeElementAt(0), lineNumber, colNumber,
        nsIScriptError::errorFlag, "malformed-xml", mInnerWindowID);
  }

  bool shouldReportError = NS_SUCCEEDED(rv);

  if (mSink && shouldReportError) {
    rv = mSink->ReportError(errorText.get(), sourceText.get(), serr,
                            &shouldReportError);
    if (NS_FAILED(rv)) {
      shouldReportError = true;
    }
  }

  if (mOriginalSink) {
    nsCOMPtr<Document> doc = do_QueryInterface(mOriginalSink->GetTarget());
    if (doc && doc->SuppressParserErrorConsoleMessages()) {
      shouldReportError = false;
    }
  }

  if (shouldReportError) {
    nsCOMPtr<nsIConsoleService> cs(do_GetService(NS_CONSOLESERVICE_CONTRACTID));
    if (cs) {
      cs->LogMessage(serr);
    }
  }

  return NS_ERROR_HTMLPARSER_STOPPARSING;
}

void nsExpatDriver::ChunkAndParseBuffer(const char16_t* aBuffer,
                                        uint32_t aLength, bool aIsFinal,
                                        uint32_t* aPassedToExpat,
                                        uint32_t* aConsumed,
                                        XML_Size* aLastLineLength) {
  *aConsumed = 0;
  *aLastLineLength = 0;

  uint32_t remainder = aLength;
  while (remainder > sMaxChunkLength) {
    ParseChunk(aBuffer, sMaxChunkLength, ChunkOrBufferIsFinal::None, aConsumed,
               aLastLineLength);
    aBuffer += sMaxChunkLength;
    remainder -= sMaxChunkLength;
    if (NS_FAILED(mInternalState)) {
      *aPassedToExpat = aLength - remainder;
      return;
    }
  }

  ParseChunk(aBuffer, remainder,
             aIsFinal ? ChunkOrBufferIsFinal::FinalChunkAndBuffer
                      : ChunkOrBufferIsFinal::FinalChunk,
             aConsumed, aLastLineLength);
  *aPassedToExpat = aLength;
}

void nsExpatDriver::ParseChunk(const char16_t* aBuffer, uint32_t aLength,
                               ChunkOrBufferIsFinal aIsFinal,
                               uint32_t* aConsumed, XML_Size* aLastLineLength) {
  NS_ASSERTION((aBuffer && aLength != 0) || (!aBuffer && aLength == 0), "?");
  NS_ASSERTION(mInternalState != NS_OK ||
                   (aIsFinal == ChunkOrBufferIsFinal::FinalChunkAndBuffer) ||
                   aBuffer,
               "Useless call, we won't call Expat");
  MOZ_ASSERT(!BlockedOrInterrupted() || !aBuffer,
             "Non-null buffer when resuming");
  MOZ_ASSERT(mExpatParser);

  int32_t parserBytesBefore = MOZ_XML_GetCurrentByteIndex(mExpatParser);
  MOZ_RELEASE_ASSERT(parserBytesBefore >= 0, "Unexpected value");
  MOZ_RELEASE_ASSERT(parserBytesBefore % sizeof(char16_t) == 0,
                     "Consumed part of a char16_t?");

  if (mInternalState != NS_OK && !BlockedOrInterrupted()) {
    return;
  }

  XML_Status status;
  bool inParser = mInParser;  
  mInParser = true;
  if (BlockedOrInterrupted()) {
    mInternalState = NS_OK;  
    status = MOZ_XML_ResumeParser(mExpatParser);
  } else {
    status = MOZ_XML_Parse(
        mExpatParser, reinterpret_cast<const char*>(aBuffer),
        aLength * sizeof(char16_t),
        aIsFinal == ChunkOrBufferIsFinal::FinalChunkAndBuffer);
  }
  mInParser = inParser;  

  int32_t parserBytesConsumed = MOZ_XML_GetCurrentByteIndex(mExpatParser);
  MOZ_RELEASE_ASSERT(parserBytesConsumed >= parserBytesBefore,
                     "Unexpected byte position");
  MOZ_RELEASE_ASSERT(parserBytesConsumed % sizeof(char16_t) == 0,
                     "Consumed part of a char16_t?");

  *aConsumed += (parserBytesConsumed - parserBytesBefore) / sizeof(char16_t);

  NS_ASSERTION(status != XML_STATUS_SUSPENDED || BlockedOrInterrupted(),
               "Inconsistent expat suspension state.");

  if (status == XML_STATUS_ERROR) {
    mInternalState = NS_ERROR_HTMLPARSER_STOPPARSING;
  }

  if (*aConsumed > 0 &&
      (aIsFinal != ChunkOrBufferIsFinal::None || NS_FAILED(mInternalState))) {
    *aLastLineLength = MOZ_XML_GetCurrentColumnNumber(mExpatParser);
  }
}

nsresult nsExpatDriver::ResumeParse(nsScanner& aScanner, bool aIsFinalChunk) {
  nsScannerIterator currentExpatPosition;
  aScanner.CurrentPosition(currentExpatPosition);

  nsScannerIterator start = currentExpatPosition;
  start.advance(mExpatBuffered);

  nsScannerIterator end;
  aScanner.EndReading(end);

  MOZ_LOG(gExpatDriverLog, LogLevel::Debug,
          ("Remaining in expat's buffer: %i, remaining in scanner: %zu.",
           mExpatBuffered, Distance(start, end)));

  while (start != end || (aIsFinalChunk && !mMadeFinalCallToExpat) ||
         (BlockedOrInterrupted() && mExpatBuffered > 0)) {
    bool noMoreBuffers = start == end && aIsFinalChunk;
    bool blocked = BlockedOrInterrupted();

    const char16_t* buffer;
    uint32_t length;
    if (blocked || noMoreBuffers) {
      buffer = nullptr;
      length = 0;

      if (blocked) {
        MOZ_LOG(
            gExpatDriverLog, LogLevel::Debug,
            ("Resuming Expat, will parse data remaining in Expat's "
             "buffer.\nContent of Expat's buffer:\n-----\n%s\n-----\n",
             NS_ConvertUTF16toUTF8(currentExpatPosition.get(), mExpatBuffered)
                 .get()));
      } else {
        NS_ASSERTION(mExpatBuffered == Distance(currentExpatPosition, end),
                     "Didn't pass all the data to Expat?");
        MOZ_LOG(
            gExpatDriverLog, LogLevel::Debug,
            ("Last call to Expat, will parse data remaining in Expat's "
             "buffer.\nContent of Expat's buffer:\n-----\n%s\n-----\n",
             NS_ConvertUTF16toUTF8(currentExpatPosition.get(), mExpatBuffered)
                 .get()));
      }
    } else {
      buffer = start.get();
      length = uint32_t(start.size_forward());

      MOZ_LOG(gExpatDriverLog, LogLevel::Debug,
              ("Calling Expat, will parse data remaining in Expat's buffer and "
               "new data.\nContent of Expat's buffer:\n-----\n%s\n-----\nNew "
               "data:\n-----\n%s\n-----\n",
               NS_ConvertUTF16toUTF8(currentExpatPosition.get(), mExpatBuffered)
                   .get(),
               NS_ConvertUTF16toUTF8(start.get(), length).get()));
    }

    uint32_t passedToExpat;
    uint32_t consumed;
    XML_Size lastLineLength;
    ChunkAndParseBuffer(buffer, length, noMoreBuffers, &passedToExpat,
                        &consumed, &lastLineLength);
    MOZ_ASSERT_IF(passedToExpat != length, NS_FAILED(mInternalState));
    MOZ_ASSERT(consumed <= passedToExpat + mExpatBuffered);
    if (consumed > 0) {
      nsScannerIterator oldExpatPosition = currentExpatPosition;
      currentExpatPosition.advance(consumed);


      if (lastLineLength <= consumed) {
        nsScannerIterator startLastLine = currentExpatPosition;
        startLastLine.advance(-((ptrdiff_t)lastLineLength));
        if (!CopyUnicodeTo(startLastLine, currentExpatPosition, mLastLine)) {
          return (mInternalState = NS_ERROR_OUT_OF_MEMORY);
        }
      } else {
        if (!AppendUnicodeTo(oldExpatPosition, currentExpatPosition,
                             mLastLine)) {
          return (mInternalState = NS_ERROR_OUT_OF_MEMORY);
        }
      }
    }

    mExpatBuffered += passedToExpat - consumed;

    if (BlockedOrInterrupted()) {
      MOZ_LOG(gExpatDriverLog, LogLevel::Debug,
              ("Blocked or interrupted parser (probably for loading linked "
               "stylesheets or scripts)."));

      aScanner.SetPosition(currentExpatPosition, true);
      aScanner.Mark();

      return mInternalState;
    }

    if (noMoreBuffers && mExpatBuffered == 0) {
      mMadeFinalCallToExpat = true;
    }

    if (NS_FAILED(mInternalState)) {
      if (MOZ_XML_GetErrorCode(mExpatParser) != XML_ERROR_NONE) {
        NS_ASSERTION(mInternalState == NS_ERROR_HTMLPARSER_STOPPARSING,
                     "Unexpected error");

        nsScannerIterator lastLine = currentExpatPosition;
        while (lastLine != end) {
          length = uint32_t(lastLine.size_forward());
          uint32_t endOffset = 0;
          const char16_t* buffer = lastLine.get();
          while (endOffset < length && buffer[endOffset] != '\n' &&
                 buffer[endOffset] != '\r') {
            ++endOffset;
          }
          mLastLine.Append(Substring(buffer, buffer + endOffset));
          if (endOffset < length) {
            break;
          }

          lastLine.advance(length);
        }

        HandleError();
      }

      return mInternalState;
    }

    NS_ASSERTION(!noMoreBuffers || blocked ||
                     (mExpatBuffered == 0 && currentExpatPosition == end),
                 "Unreachable data left in Expat's buffer");

    start.advance(length);

    aScanner.EndReading(end);
  }

  aScanner.SetPosition(currentExpatPosition, true);
  aScanner.Mark();

  MOZ_LOG(gExpatDriverLog, LogLevel::Debug,
          ("Remaining in expat's buffer: %i, remaining in scanner: %zu.",
           mExpatBuffered, Distance(currentExpatPosition, end)));

  return NS_SUCCEEDED(mInternalState) ? NS_ERROR_HTMLPARSER_EOF : NS_OK;
}

nsresult nsExpatDriver::Initialize(nsIURI* aURI, nsIContentSink* aSink) {
  mSink = do_QueryInterface(aSink);
  if (!mSink) {
    NS_ERROR("nsExpatDriver didn't get an nsIExpatSink");
    mInternalState = NS_ERROR_UNEXPECTED;
    return mInternalState;
  }

  mOriginalSink = aSink;

  static const char16_t kExpatSeparator[] = {kExpatSeparatorChar, '\0'};

  nsCOMPtr<Document> doc = do_QueryInterface(mOriginalSink->GetTarget());
  if (doc) {
    nsCOMPtr<nsPIDOMWindowOuter> win = doc->GetWindow();
    nsCOMPtr<nsPIDOMWindowInner> inner;
    if (win) {
      inner = win->GetCurrentInnerWindow();
    } else {
      bool aHasHadScriptHandlingObject;
      nsIScriptGlobalObject* global =
          doc->GetScriptHandlingObject(aHasHadScriptHandlingObject);
      if (global) {
        inner = do_QueryInterface(global);
      }
    }
    if (inner) {
      mInnerWindowID = inner->WindowID();
    }
  }

  mExpatParser =
      MOZ_XML_ParserCreate_MM(kUTF16, nullptr, kExpatSeparator);
  NS_ENSURE_TRUE(mExpatParser, NS_ERROR_FAILURE);

  MOZ_XML_SetReturnNSTriplet(mExpatParser, XML_TRUE);

#ifdef XML_DTD
  MOZ_XML_SetParamEntityParsing(mExpatParser, XML_PARAM_ENTITY_PARSING_ALWAYS);
#endif

  char salt[16];
  MOZ_RELEASE_ASSERT(mozilla::GenerateRandomBytesFromOS(salt, sizeof(salt)));
  MOZ_RELEASE_ASSERT(
      MOZ_XML_SetHashSalt16Bytes(mExpatParser, salt));
  MOZ_RELEASE_ASSERT(
      MOZ_XML_SetReparseDeferralEnabled(mExpatParser, XML_FALSE));

  auto baseURI = GetExpatBaseURI(aURI);
  MOZ_XML_SetBase(mExpatParser, baseURI.begin());

  MOZ_XML_SetXmlDeclHandler(mExpatParser, Driver_HandleXMLDeclaration);
  if (doc && doc->NodePrincipal()->IsSystemPrincipal()) {
    MOZ_XML_SetElementHandler(mExpatParser,
                              HandleStartElementForSystemPrincipal,
                              HandleEndElementForSystemPrincipal);
  } else {
    MOZ_XML_SetElementHandler(mExpatParser, HandleStartElement,
                              HandleEndElement);
  }
  MOZ_XML_SetCharacterDataHandler(mExpatParser, Driver_HandleCharacterData);
  MOZ_XML_SetProcessingInstructionHandler(
      mExpatParser, Driver_HandleProcessingInstruction);
  MOZ_XML_SetDefaultHandlerExpand(mExpatParser, Driver_HandleDefault);
  MOZ_XML_SetExternalEntityRefHandler(
      mExpatParser,
      reinterpret_cast<XML_ExternalEntityRefHandler>(
          Driver_HandleExternalEntityRef));
  XML_SetExternalEntityRefHandlerArg(mExpatParser, this);
  MOZ_XML_SetCommentHandler(mExpatParser, Driver_HandleComment);
  MOZ_XML_SetCdataSectionHandler(mExpatParser, Driver_HandleStartCdataSection,
                                 Driver_HandleEndCdataSection);
  MOZ_XML_SetParamEntityParsing(
      mExpatParser, XML_PARAM_ENTITY_PARSING_UNLESS_STANDALONE);
  MOZ_XML_SetDoctypeDeclHandler(mExpatParser, Driver_HandleStartDoctypeDecl,
                                Driver_HandleEndDoctypeDecl);
  XML_SetUserData(mExpatParser, this);

  return mInternalState;
}

nsresult nsExpatDriver::BuildModel() { return mInternalState; }

void nsExpatDriver::DidBuildModel() {
  if (!mInParser) {
    Destroy();
  }
  mOriginalSink = nullptr;
  mSink = nullptr;
}

void nsExpatDriver::Terminate() {
  if (mExpatParser) {
    MOZ_XML_StopParser(mExpatParser, XML_FALSE);
  }
  mInternalState = NS_ERROR_HTMLPARSER_STOPPARSING;
}


void nsExpatDriver::MaybeStopParser(nsresult aState) {
  if (NS_FAILED(aState)) {
    if (NS_SUCCEEDED(mInternalState) ||
        mInternalState == NS_ERROR_HTMLPARSER_INTERRUPTED ||
        (mInternalState == NS_ERROR_HTMLPARSER_BLOCK &&
         aState != NS_ERROR_HTMLPARSER_INTERRUPTED)) {
      mInternalState = (aState == NS_ERROR_HTMLPARSER_INTERRUPTED ||
                        aState == NS_ERROR_HTMLPARSER_BLOCK)
                           ? aState
                           : NS_ERROR_HTMLPARSER_STOPPARSING;
    }


    int resumable = BlockedOrInterrupted();
    MOZ_XML_StopParser(mExpatParser, resumable);
  } else if (NS_SUCCEEDED(mInternalState)) {
    mInternalState = aState;
  }
}

nsExpatDriver::ExpatBaseURI nsExpatDriver::GetExpatBaseURI(nsIURI* aURI) {
  mURIs.AppendElement(aURI);

  MOZ_RELEASE_ASSERT(mURIs.Length() <= std::numeric_limits<XML_Char>::max());

  return ExpatBaseURI(static_cast<XML_Char>(mURIs.Length()), XML_T('\0'));
}

nsIURI* nsExpatDriver::GetBaseURI(const XML_Char* aBase) const {
  MOZ_ASSERT(aBase[0] != '\0' && aBase[1] == '\0');

  if (aBase[0] == '\0' || aBase[1] != '\0') {
    return nullptr;
  }

  uint32_t index = aBase[0] - 1;
  MOZ_ASSERT(index < mURIs.Length());

  return mURIs.SafeElementAt(index);
}

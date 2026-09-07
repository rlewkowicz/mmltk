/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NS_EXPAT_DRIVER_
#define NS_EXPAT_DRIVER_

#include "expat_config.h"
#include "moz_expat.h"
#include "nsCOMPtr.h"
#include "nsString.h"
#include "nsIInputStream.h"
#include "nsIParser.h"
#include "nsCycleCollectionParticipant.h"
#include "nsScanner.h"

class nsIExpatSink;
struct nsCatalogData;
namespace mozilla {
template <typename, size_t>
class Array;
}

class nsExpatDriver : public nsISupports {
  virtual ~nsExpatDriver();

 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS_FINAL
  NS_DECL_CYCLE_COLLECTION_CLASS(nsExpatDriver)

  nsExpatDriver();

  nsresult Initialize(nsIURI* aURI, nsIContentSink* aSink);

  void DidBuildModel();
  nsresult BuildModel();
  void Terminate();

  nsresult ResumeParse(nsScanner& aScanner, bool aIsFinalChunk);

  int HandleExternalEntityRef(const char16_t* aOpenEntityNames,
                              const char16_t* aBase, const char16_t* aSystemId,
                              const char16_t* aPublicId);
  static void HandleStartElement(void* aUserData, const char16_t* aName,
                                 const char16_t** aAtts);
  static void HandleStartElementForSystemPrincipal(
      void* aUserData, const char16_t* aName, const char16_t** aAtts);
  static void HandleEndElement(void* aUserData, const char16_t* aName);
  static void HandleEndElementForSystemPrincipal(
      void* aUserData, const char16_t* aName);
  nsresult HandleCharacterData(const char16_t* aCData, const uint32_t aLength);
  nsresult HandleComment(const char16_t* aName);
  nsresult HandleProcessingInstruction(const char16_t* aTarget,
                                       const char16_t* aData);
  nsresult HandleXMLDeclaration(const char16_t* aVersion,
                                const char16_t* aEncoding, int32_t aStandalone);
  nsresult HandleDefault(const char16_t* aData, const uint32_t aLength);
  nsresult HandleStartCdataSection();
  nsresult HandleEndCdataSection();
  nsresult HandleStartDoctypeDecl(const char16_t* aDoctypeName,
                                  const char16_t* aSysid,
                                  const char16_t* aPubid,
                                  bool aHasInternalSubset);
  nsresult HandleEndDoctypeDecl();

 private:
  nsresult OpenInputStreamFromExternalDTD(const char16_t* aFPIStr,
                                          const char16_t* aURLStr,
                                          nsIURI* aBaseURI,
                                          nsIInputStream** aStream,
                                          nsIURI** aAbsURI);

  enum class ChunkOrBufferIsFinal {
    None,
    FinalChunk,
    FinalChunkAndBuffer,
  };

  void ParseChunk(const char16_t* aBuffer, uint32_t aLength,
                  ChunkOrBufferIsFinal aIsFinal, uint32_t* aConsumed,
                  XML_Size* aLastLineLength);
  void ChunkAndParseBuffer(const char16_t* aBuffer, uint32_t aLength,
                           bool aIsFinal, uint32_t* aPassedToExpat,
                           uint32_t* aConsumed, XML_Size* aLastLineLength);

  nsresult HandleError();

  void MaybeStopParser(nsresult aState);

  bool BlockedOrInterrupted() {
    return mInternalState == NS_ERROR_HTMLPARSER_BLOCK ||
           mInternalState == NS_ERROR_HTMLPARSER_INTERRUPTED;
  }

  using ExpatBaseURI = mozilla::Array<XML_Char, 2>;
  ExpatBaseURI GetExpatBaseURI(nsIURI* aURI);
  nsIURI* GetBaseURI(const XML_Char* aBase) const;

  void Destroy();

  XML_Parser mExpatParser;

  nsString mLastLine;
  nsString mCDataText;
  nsString mDoctypeName;
  nsString mSystemID;
  nsString mPublicID;
  nsString mInternalSubset;
  bool mInCData;
  bool mInInternalSubset;
  bool mInExternalDTD;
  bool mMadeFinalCallToExpat;

  bool mInParser;

  nsresult mInternalState;

  uint32_t mExpatBuffered;

  uint16_t mTagDepth;

  nsCOMPtr<nsIContentSink> mOriginalSink;
  nsCOMPtr<nsIExpatSink> mSink;

  const nsCatalogData* mCatalogData;  
  nsTArray<nsCOMPtr<nsIURI>> mURIs;

  uint64_t mInnerWindowID;
};

#endif

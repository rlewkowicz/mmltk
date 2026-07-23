// License & terms of use: http://www.unicode.org/copyright.html
/**
 *******************************************************************************
 * Copyright (C) 2001-2011, International Business Machines Corporation.       *
 * All Rights Reserved.                                                        *
 *******************************************************************************
 */

#ifndef ICUSERV_H
#define ICUSERV_H

#include "unicode/utypes.h"

#if UCONFIG_NO_SERVICE

U_NAMESPACE_BEGIN

class ICUService;

U_NAMESPACE_END

#else

#include "unicode/unistr.h"
#include "unicode/locid.h"
#include "unicode/umisc.h"

#include "hash.h"
#include "uvector.h"
#include "servnotf.h"

class ICUServiceTest;

U_NAMESPACE_BEGIN

class ICUServiceKey;
class ICUServiceFactory;
class SimpleFactory;
class ServiceListener;
class ICUService;

class DNCache;


class U_COMMON_API ICUServiceKey : public UObject {
 private: 
  const UnicodeString _id;

 protected:
  static const char16_t PREFIX_DELIMITER;

 public:

  ICUServiceKey(const UnicodeString& id);

  virtual ~ICUServiceKey();

  virtual const UnicodeString& getID() const;

  virtual UnicodeString& canonicalID(UnicodeString& result) const;

  virtual UnicodeString& currentID(UnicodeString& result) const;

  virtual UnicodeString& currentDescriptor(UnicodeString& result) const;

  virtual UBool fallback();

  virtual UBool isFallbackOf(const UnicodeString& id) const;

  virtual UnicodeString& prefix(UnicodeString& result) const;

  static UnicodeString& parsePrefix(UnicodeString& result);

  static UnicodeString& parseSuffix(UnicodeString& result);

public:
  static UClassID U_EXPORT2 getStaticClassID();

  virtual UClassID getDynamicClassID() const override;

#ifdef SERVICE_DEBUG
 public:
  virtual UnicodeString& debug(UnicodeString& result) const;
  virtual UnicodeString& debugClass(UnicodeString& result) const;
#endif

};


class U_COMMON_API ICUServiceFactory : public UObject {
 public:
    virtual ~ICUServiceFactory();

    virtual UObject* create(const ICUServiceKey& key, const ICUService* service, UErrorCode& status) const = 0;

    virtual void updateVisibleIDs(Hashtable& result, UErrorCode& status) const = 0;

    virtual UnicodeString& getDisplayName(const UnicodeString& id, const Locale& locale, UnicodeString& result) const = 0;
};


class U_COMMON_API SimpleFactory : public ICUServiceFactory {
 protected:
  UObject* _instance;
  const UnicodeString _id;
  const UBool _visible;

 public:
  SimpleFactory(UObject* instanceToAdopt, const UnicodeString& id, UBool visible = true);

  virtual ~SimpleFactory();

  virtual UObject* create(const ICUServiceKey& key, const ICUService* service, UErrorCode& status) const override;

  virtual void updateVisibleIDs(Hashtable& result, UErrorCode& status) const override;

  virtual UnicodeString& getDisplayName(const UnicodeString& id, const Locale& locale, UnicodeString& result) const override;

public:
  static UClassID U_EXPORT2 getStaticClassID();

  virtual UClassID getDynamicClassID() const override;

#ifdef SERVICE_DEBUG
 public:
  virtual UnicodeString& debug(UnicodeString& toAppendTo) const;
  virtual UnicodeString& debugClass(UnicodeString& toAppendTo) const;
#endif

};


class U_COMMON_API ServiceListener : public EventListener {
public:
    virtual ~ServiceListener();

    virtual void serviceChanged(const ICUService& service) const = 0;
    
public:
    static UClassID U_EXPORT2 getStaticClassID();
    
    virtual UClassID getDynamicClassID() const override;
    
};


class U_COMMON_API StringPair : public UMemory {
public:
  const UnicodeString displayName;

  const UnicodeString id;

  static StringPair* create(const UnicodeString& displayName, 
                            const UnicodeString& id,
                            UErrorCode& status);

  UBool isBogus() const;

private:
  StringPair(const UnicodeString& displayName, const UnicodeString& id);
};


 /**
 * <p>A Service provides access to service objects that implement a
 * particular service, e.g. transliterators.  Users provide a String
 * id (for example, a locale string) to the service, and get back an
 * object for that id.  Service objects can be any kind of object.  A
 * new service object is returned for each query. The caller is
 * responsible for deleting it.</p>
 *
 * <p>Services 'canonicalize' the query ID and use the canonical ID to
 * query for the service.  The service also defines a mechanism to
 * 'fallback' the ID multiple times.  Clients can optionally request
 * the actual ID that was matched by a query when they use an ID to
 * retrieve a service object.</p>
 *
 * <p>Service objects are instantiated by ICUServiceFactory objects
 * registered with the service.  The service queries each
 * ICUServiceFactory in turn, from most recently registered to
 * earliest registered, until one returns a service object.  If none
 * responds with a service object, a fallback ID is generated, and the
 * process repeats until a service object is returned or until the ID
 * has no further fallbacks.</p>
 *
 * <p>In ICU 2.4, UObject (the base class of service instances) does
 * not define a polymorphic clone function.  ICUService uses clones to
 * manage ownership.  Thus, for now, ICUService defines an abstract
 * method, cloneInstance, that clients must implement to create clones
 * of the service instances.  This may change in future releases of
 * ICU.</p>
 *
 * <p>ICUServiceFactories can be dynamically registered and
 * unregistered with the service.  When registered, an
 * ICUServiceFactory is installed at the head of the factory list, and
 * so gets 'first crack' at any keys or fallback keys.  When
 * unregistered, it is removed from the service and can no longer be
 * located through it.  Service objects generated by this factory and
 * held by the client are unaffected.</p>
 *
 * <p>If a service has variants (e.g., the different variants of
 * BreakIterator) an ICUServiceFactory can use the prefix of the
 * ICUServiceKey to determine the variant of a service to generate.
 * If it does not support all variants, it can request
 * previously-registered factories to handle the ones it does not
 * support.</p>
 *
 * <p>ICUService uses ICUServiceKeys to query factories and perform
 * fallback.  The ICUServiceKey defines the canonical form of the ID,
 * and implements the fallback strategy.  Custom ICUServiceKeys can be
 * defined that parse complex IDs into components that
 * ICUServiceFactories can more easily use.  The ICUServiceKey can
 * cache the results of this parsing to save repeated effort.
 * ICUService provides convenience APIs that take UnicodeStrings and
 * generate default ICUServiceKeys for use in querying.</p>
 *
 * <p>ICUService provides API to get the list of IDs publicly
 * supported by the service (although queries aren't restricted to
 * this list).  This list contains only 'simple' IDs, and not fully
 * unique IDs.  ICUServiceFactories are associated with each simple ID
 * and the responsible factory can also return a human-readable
 * localized version of the simple ID, for use in user interfaces.
 * ICUService can also provide an array of the all the localized
 * visible IDs and their corresponding internal IDs.</p>
 *
 * <p>ICUService implements ICUNotifier, so that clients can register
 * to receive notification when factories are added or removed from
 * the service.  ICUService provides a default EventListener
 * subinterface, ServiceListener, which can be registered with the
 * service.  When the service changes, the ServiceListener's
 * serviceChanged method is called with the service as the
 * argument.</p>
 *
 * <p>The ICUService API is both rich and generic, and it is expected
 * that most implementations will statically 'wrap' ICUService to
 * present a more appropriate API-- for example, to declare the type
 * of the objects returned from get, to limit the factories that can
 * be registered with the service, or to define their own listener
 * interface with a custom callback method.  They might also customize
 * ICUService by overriding it, for example, to customize the
 * ICUServiceKey and fallback strategy.  ICULocaleService is a
 * subclass of ICUService that uses Locale names as IDs and uses
 * ICUServiceKeys that implement the standard resource bundle fallback
 * strategy.  Most clients will wish to subclass it instead of
 * ICUService.</p> 
 */
class U_COMMON_API ICUService : public ICUNotifier {
 protected: 
    const UnicodeString name;

 private:

    uint32_t timestamp;

    UVector* factories;

    Hashtable* serviceCache;

    Hashtable* idCache;

    DNCache* dnCache;

 public:
    ICUService();

    ICUService(const UnicodeString& name);

    virtual ~ICUService();

    UnicodeString& getName(UnicodeString& result) const;

    UObject* get(const UnicodeString& descriptor, UErrorCode& status) const;

    UObject* get(const UnicodeString& descriptor, UnicodeString* actualReturn, UErrorCode& status) const;

    UObject* getKey(ICUServiceKey& key, UErrorCode& status) const;

    virtual UObject* getKey(ICUServiceKey& key, UnicodeString* actualReturn, UErrorCode& status) const;

    UObject* getKey(ICUServiceKey& key, UnicodeString* actualReturn, const ICUServiceFactory* factory, UErrorCode& status) const;

    UVector& getVisibleIDs(UVector& result, UErrorCode& status) const;

    UVector& getVisibleIDs(UVector& result, const UnicodeString* matchID, UErrorCode& status) const;

    UnicodeString& getDisplayName(const UnicodeString& id, UnicodeString& result) const;

    UnicodeString& getDisplayName(const UnicodeString& id, UnicodeString& result, const Locale& locale) const;

    UVector& getDisplayNames(UVector& result, UErrorCode& status) const;

    UVector& getDisplayNames(UVector& result, const Locale& locale, UErrorCode& status) const;

    UVector& getDisplayNames(UVector& result,
                             const Locale& locale, 
                             const UnicodeString* matchID, 
                             UErrorCode& status) const;

    URegistryKey registerInstance(UObject* objToAdopt, const UnicodeString& id, UErrorCode& status);

    virtual URegistryKey registerInstance(UObject* objToAdopt, const UnicodeString& id, UBool visible, UErrorCode& status);

    virtual URegistryKey registerFactory(ICUServiceFactory* factoryToAdopt, UErrorCode& status);

    virtual UBool unregister(URegistryKey rkey, UErrorCode& status);

    virtual void reset();

    virtual UBool isDefault() const;

    virtual ICUServiceKey* createKey(const UnicodeString* id, UErrorCode& status) const;

    virtual UObject* cloneInstance(UObject* instance) const = 0;



 protected:

    virtual ICUServiceFactory* createSimpleFactory(UObject* instanceToAdopt, const UnicodeString& id, UBool visible, UErrorCode& status);

    virtual void reInitializeFactories();

    virtual UObject* handleDefault(const ICUServiceKey& key, UnicodeString* actualReturn, UErrorCode& status) const;

    virtual void clearCaches();

    virtual UBool acceptsListener(const EventListener& l) const override;

    virtual void notifyListener(EventListener& l) const override;


    void clearServiceCache();

    const Hashtable* getVisibleIDMap(UErrorCode& status) const;

    int32_t getTimestamp() const;

    int32_t countFactories() const;

private:

    friend class ::ICUServiceTest; 
};

U_NAMESPACE_END

#endif

#endif


/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PKCS11ModuleDB.h"

#include "CertVerifier.h"
#include "PKCS11Module.h"
#include "ScopedNSSTypes.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/StaticPrefs_security.h"
#include "mozilla/dom/Promise.h"
#include "nsComponentManagerUtils.h"
#include "nsNSSCertHelper.h"
#include "nsNSSComponent.h"
#include "nsNativeCharsetUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "nss.h"
#include "pk11pub.h"
#include "xpcpublic.h"

using mozilla::ErrorResult;
using mozilla::dom::Promise;

namespace mozilla {
namespace psm {

NS_IMPL_ISUPPORTS(PKCS11ModuleDB, nsIPKCS11ModuleDB)

PKCS11ModuleDB::PKCS11ModuleDB() {
  MOZ_ASSERT(NS_IsMainThread());

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  if (StaticPrefs::security_utility_pkcs11_module_process_enabled() &&
      !GetInSafeMode()) {
    auto manager = ipc::UtilityProcessManager::GetSingleton();
    MOZ_ASSERT(manager);
    if (manager) {
      mPKCS11ModuleProcessPromise = manager->StartPKCS11Module()->Then(
          GetCurrentSerialEventTarget(), __func__,
          [](RefPtr<PKCS11ModuleParent>&& parent) {
            MOZ_RELEASE_ASSERT(parent);
            return PKCS11ModuleProcessPromise::CreateAndResolve(
                std::move(parent), __func__);
          },
          [](base::LaunchError&& _) {
            return PKCS11ModuleProcessPromise::CreateAndReject(NS_ERROR_FAILURE,
                                                               __func__);
          });
    }
  }
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS
}

StaticRefPtr<PKCS11ModuleDB> sPKCS11ModuleDB;

already_AddRefed<PKCS11ModuleDB> PKCS11ModuleDB::GetSingleton() {
  MOZ_ASSERT(NS_IsMainThread());
  if (!NS_IsMainThread()) {
    return nullptr;
  }

  if (!sPKCS11ModuleDB) {
    sPKCS11ModuleDB = new PKCS11ModuleDB();
    ClearOnShutdown(&sPKCS11ModuleDB);
  }

  return do_AddRef(sPKCS11ModuleDB);
}

nsresult DispatchToNSSTaskQueue(already_AddRefed<nsIRunnable> aRunnable) {
  nsCOMPtr<nsIRunnable> runnable(aRunnable);
  nsCOMPtr<nsINSSComponent> nss(do_GetService(PSM_COMPONENT_CONTRACTID));
  if (!nss) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  nsCOMPtr<nsISerialEventTarget> nssTaskQueue;
  nsresult rv = nss->GetNssTaskQueue(getter_AddRefs(nssTaskQueue));
  if (NS_FAILED(rv)) {
    return rv;
  }
  return nssTaskQueue->Dispatch(runnable.forget());
}

static nsresult NormalizeModuleNameIn(const nsAString& moduleNameIn,
                                      nsCString& moduleNameOut) {
  nsAutoString localizedRootModuleName;
  nsresult rv =
      GetPIPNSSBundleString("RootCertModuleName", localizedRootModuleName);
  if (NS_FAILED(rv)) {
    return rv;
  }
  if (moduleNameIn.Equals(localizedRootModuleName)) {
    moduleNameOut.Assign(kRootModuleName.get());
    return NS_OK;
  }
  moduleNameOut.Assign(NS_ConvertUTF16toUTF8(moduleNameIn));
  return NS_OK;
}

nsresult PKCS11ModuleDB::DoDeleteModule(const nsCString& moduleName) {
  if (!NSS_IsInitialized()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  int32_t modType;
  SECStatus srv = SECMOD_DeleteModule(moduleName.get(), &modType);
  if (srv != SECSuccess) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

NS_IMETHODIMP
PKCS11ModuleDB::DeleteModule(const nsAString& aModuleName, JSContext* aCx,
                             Promise** aPromise) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!NS_IsMainThread()) {
    return NS_ERROR_NOT_SAME_THREAD;
  }
  if (aModuleName.IsEmpty()) {
    return NS_ERROR_INVALID_ARG;
  }

  nsAutoCString moduleNameNormalized;
  nsresult rv = NormalizeModuleNameIn(aModuleName, moduleNameNormalized);
  if (NS_FAILED(rv)) {
    return rv;
  }

  ErrorResult result;
  RefPtr<Promise> promise =
      Promise::Create(xpc::CurrentNativeGlobal(aCx), result);
  if (result.Failed()) {
    return result.StealNSResult();
  }
  auto promiseHolder = MakeRefPtr<nsMainThreadPtrHolder<Promise>>(
      "DeleteModule promise", promise);

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  if (StaticPrefs::security_utility_pkcs11_module_process_enabled()) {
    if (!mPKCS11ModuleProcessPromise) {
      return NS_ERROR_NOT_AVAILABLE;
    }
    mPKCS11ModuleProcessPromise->Then(
        GetCurrentSerialEventTarget(), __func__,
        [promiseHolder, moduleNameNormalized = std::move(moduleNameNormalized)](
            const RefPtr<PKCS11ModuleParent>& parent) {
          MOZ_RELEASE_ASSERT(parent);
          parent->SendDeleteModule(moduleNameNormalized)
              ->Then(GetCurrentSerialEventTarget(), __func__,
                     [promiseHolder](
                         const PPKCS11ModuleParent::DeleteModulePromise::
                             ResolveOrRejectValue& value) {
                       RefPtr<SharedCertVerifier> certVerifier(
                           GetDefaultCertVerifier());
                       if (certVerifier) {
                         certVerifier->ClearTrustCache();
                       }
                       if (value.IsResolve()) {
                         nsresult rv = value.ResolveValue();
                         if (NS_SUCCEEDED(rv)) {
                           promiseHolder->get()->MaybeResolveWithUndefined();
                         } else {
                           promiseHolder->get()->MaybeReject(rv);
                         }
                       } else {
                         promiseHolder->get()->MaybeReject(NS_ERROR_FAILURE);
                       }
                     });
        },
        [promiseHolder](nsresult rv) {
          promiseHolder->get()->MaybeReject(rv);
        });
    promise.forget(aPromise);
    return NS_OK;
  }
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS

  nsCOMPtr<nsIRunnable> runnable(NS_NewRunnableFunction(
      "DeleteModule runnable", [promiseHolder, moduleNameNormalized = std::move(
                                                   moduleNameNormalized)]() {
        nsresult rv = DoDeleteModule(moduleNameNormalized);
        RefPtr<SharedCertVerifier> certVerifier(GetDefaultCertVerifier());
        if (certVerifier) {
          certVerifier->ClearTrustCache();
        }
        NS_DispatchToMainThread(NS_NewRunnableFunction(
            "DeleteModule callback", [rv, promiseHolder] {
              if (NS_SUCCEEDED(rv)) {
                promiseHolder->get()->MaybeResolveWithUndefined();
              } else {
                promiseHolder->get()->MaybeReject(rv);
              }
            }));
      }));

  promise.forget(aPromise);
  return DispatchToNSSTaskQueue(runnable.forget());
}

nsresult PKCS11ModuleDB::DoAddModule(const nsCString& moduleName,
                                     const nsCString& libraryPath,
                                     uint32_t mechanismFlags,
                                     uint32_t cipherFlags) {
  if (!NSS_IsInitialized()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  uint32_t internalMechanismFlags =
      SECMOD_PubMechFlagstoInternal(mechanismFlags);
  uint32_t internalCipherFlags = SECMOD_PubCipherFlagstoInternal(cipherFlags);
  SECStatus srv =
      SECMOD_AddNewModule(moduleName.get(), libraryPath.get(),
                          internalMechanismFlags, internalCipherFlags);
  if (srv != SECSuccess) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

NS_IMETHODIMP
PKCS11ModuleDB::AddModule(const nsAString& aModuleName,
                          const nsAString& aLibraryPath,
                          uint32_t aMechanismFlags, uint32_t aCipherFlags,
                          JSContext* aCx, Promise** aPromise) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!NS_IsMainThread()) {
    return NS_ERROR_NOT_SAME_THREAD;
  }
  if (aModuleName.IsEmpty()) {
    return NS_ERROR_INVALID_ARG;
  }

  if (aModuleName.EqualsLiteral("Root Certs")) {
    return NS_ERROR_ILLEGAL_VALUE;
  }

  nsAutoCString moduleNameNormalized;
  nsresult rv = NormalizeModuleNameIn(aModuleName, moduleNameNormalized);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsAutoCString libraryPath;
  CopyUTF16toUTF8(aLibraryPath, libraryPath);

  ErrorResult result;
  RefPtr<Promise> promise =
      Promise::Create(xpc::CurrentNativeGlobal(aCx), result);
  if (result.Failed()) {
    return result.StealNSResult();
  }
  auto promiseHolder =
      MakeRefPtr<nsMainThreadPtrHolder<Promise>>("AddModule promise", promise);

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  if (StaticPrefs::security_utility_pkcs11_module_process_enabled()) {
    if (!mPKCS11ModuleProcessPromise) {
      return NS_ERROR_NOT_AVAILABLE;
    }
    mPKCS11ModuleProcessPromise->Then(
        GetCurrentSerialEventTarget(), __func__,
        [promiseHolder, moduleNameNormalized, libraryPath, aMechanismFlags,
         aCipherFlags](const RefPtr<PKCS11ModuleParent>& parent) {
          parent
              ->SendAddModule(moduleNameNormalized, libraryPath,
                              aMechanismFlags, aCipherFlags)
              ->Then(
                  GetCurrentSerialEventTarget(), __func__,
                  [promiseHolder](const PPKCS11ModuleParent::AddModulePromise::
                                      ResolveOrRejectValue& value) {
                    RefPtr<SharedCertVerifier> certVerifier(
                        GetDefaultCertVerifier());
                    if (certVerifier) {
                      certVerifier->ClearTrustCache();
                    }
                    if (value.IsResolve()) {
                      nsresult rv = value.ResolveValue();
                      if (NS_SUCCEEDED(rv)) {
                        promiseHolder->get()->MaybeResolveWithUndefined();
                      } else {
                        promiseHolder->get()->MaybeReject(rv);
                      }
                    } else {
                      promiseHolder->get()->MaybeReject(NS_ERROR_FAILURE);
                    }
                  });
        },
        [promiseHolder](nsresult rv) {
          promiseHolder->get()->MaybeReject(rv);
        });
    promise.forget(aPromise);
    return NS_OK;
  }
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS

  nsCOMPtr<nsIRunnable> runnable(NS_NewRunnableFunction(
      "AddModule runnable",
      [promiseHolder, moduleNameNormalized = std::move(moduleNameNormalized),
       libraryPath = std::move(libraryPath), mechanismFlags = aMechanismFlags,
       cipherFlags = aCipherFlags]() {
        nsresult rv = DoAddModule(moduleNameNormalized, libraryPath,
                                  mechanismFlags, cipherFlags);
        RefPtr<SharedCertVerifier> certVerifier(GetDefaultCertVerifier());
        if (certVerifier) {
          certVerifier->ClearTrustCache();
        }
        NS_DispatchToMainThread(
            NS_NewRunnableFunction("AddModule callback", [rv, promiseHolder] {
              if (NS_SUCCEEDED(rv)) {
                promiseHolder->get()->MaybeResolveWithUndefined();
              } else {
                promiseHolder->get()->MaybeReject(rv);
              }
            }));
      }));

  promise.forget(aPromise);
  return DispatchToNSSTaskQueue(runnable.forget());
}

nsresult CollectModules(nsTArray<UniqueSECMODModule>& modules) {
  if (!NSS_IsInitialized()) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  modules.Clear();

  AutoSECMODListReadLock lock;
  for (SECMODModuleList* list = SECMOD_GetDefaultModuleList(); list;
       list = list->next) {
    modules.AppendElement(SECMOD_ReferenceModule(list->module));
  }
  for (SECMODModuleList* list = SECMOD_GetDeadModuleList(); list;
       list = list->next) {
    modules.AppendElement(SECMOD_ReferenceModule(list->module));
  }
  return NS_OK;
}

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
nsresult PKCS11ModuleDB::DoListModules(nsTArray<ModuleInfo>& modules) {
  modules.Clear();
  nsTArray<UniqueSECMODModule> rawModules;
  nsresult rv = CollectModules(rawModules);
  if (NS_FAILED(rv)) {
    return rv;
  }

  for (const auto& rawModule : rawModules) {
    if (rawModule.get() == SECMOD_GetInternalModule()) {
      continue;
    }
    RefPtr<PKCS11Module> module(new PKCS11Module(rawModule.get()));
    ModuleInfo moduleInfo;
    rv = module->GetModuleInfo(moduleInfo);
    if (NS_FAILED(rv)) {
      return rv;
    }
    modules.AppendElement(std::move(moduleInfo));
  }

  return NS_OK;
}
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS

RefPtr<PKCS11ModuleDB::ListModulesPromise>
PKCS11ModuleDB::ListMainProcessModules() {
  nsCOMPtr<nsINSSComponent> nss(do_GetService(PSM_COMPONENT_CONTRACTID));
  if (!nss) {
    return ListModulesPromise::CreateAndReject(NS_ERROR_NOT_AVAILABLE,
                                               __func__);
  }
  nsCOMPtr<nsISerialEventTarget> nssTaskQueue;
  nsresult rv = nss->GetNssTaskQueue(getter_AddRefs(nssTaskQueue));
  if (NS_FAILED(rv)) {
    return ListModulesPromise::CreateAndReject(rv, __func__);
  }
  nsCOMPtr<nsISerialEventTarget> mainThread(do_GetMainThread());
  using ListRawModulesPromise =
      MozPromise<nsTArray<UniqueSECMODModule>, nsresult, true>;
  return InvokeAsync(nssTaskQueue, __func__,
                     []() -> RefPtr<ListRawModulesPromise> {
                       nsTArray<UniqueSECMODModule> rawModules;
                       nsresult rv = CollectModules(rawModules);
                       if (NS_FAILED(rv)) {
                         return ListRawModulesPromise::CreateAndReject(
                             rv, __func__);
                       }
                       return ListRawModulesPromise::CreateAndResolve(
                           std::move(rawModules), __func__);
                     })
      ->Then(
          mainThread, __func__,
          [](nsTArray<UniqueSECMODModule> rawModules) {
            nsTArray<RefPtr<nsIPKCS11Module>> modules;
            for (const auto& rawModule : rawModules) {
              modules.AppendElement(new PKCS11Module(rawModule.get()));
            }
            return ListModulesPromise::CreateAndResolve(std::move(modules),
                                                        __func__);
          },
          [](nsresult rv) {
            return ListModulesPromise::CreateAndReject(rv, __func__);
          });
}

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
RefPtr<PKCS11ModuleDB::ListModulesPromise>
PKCS11ModuleDB::ListRemoteProcessModulesGivenParent(
    const RefPtr<PKCS11ModuleParent>& parent) {
  using ReturnType = std::tuple<const nsresult&, nsTArray<ModuleInfo>&&>;
  return parent->SendListModules()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [](ReturnType rv) {
        if (NS_FAILED(std::get<0>(rv))) {
          return ListModulesPromise::CreateAndReject(std::get<0>(rv), __func__);
        }
        nsTArray<RefPtr<nsIPKCS11Module>> modules;
        for (const auto& moduleInfo : std::get<1>(rv)) {
          modules.AppendElement(new RemotePKCS11Module(moduleInfo));
        }
        return ListModulesPromise::CreateAndResolve(std::move(modules),
                                                    __func__);
      },
      [](ipc::ResponseRejectReason reason) {
        return ListModulesPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
      });
}

RefPtr<PKCS11ModuleDB::ListModulesPromise>
PKCS11ModuleDB::ListRemoteProcessModules() {
  if (!mPKCS11ModuleProcessPromise) {
    return ListModulesPromise::CreateAndReject(NS_ERROR_NOT_AVAILABLE,
                                               __func__);
  }
  return mPKCS11ModuleProcessPromise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [](const RefPtr<PKCS11ModuleParent>& parent) {
        MOZ_RELEASE_ASSERT(parent);
        return ListRemoteProcessModulesGivenParent(parent);
      },
      [](nsresult rv) {
        return ListModulesPromise::CreateAndReject(rv, __func__);
      });
}
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS

NS_IMETHODIMP
PKCS11ModuleDB::ListModules(JSContext* aCx, Promise** aPromise) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!NS_IsMainThread()) {
    return NS_ERROR_NOT_SAME_THREAD;
  }

  nsTArray<RefPtr<ListModulesPromise>> promises;
  promises.AppendElement(ListMainProcessModules());

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  if (StaticPrefs::security_utility_pkcs11_module_process_enabled()) {
    promises.AppendElement(ListRemoteProcessModules());
  }
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS

  ErrorResult result;
  RefPtr<Promise> promise =
      Promise::Create(xpc::CurrentNativeGlobal(aCx), result);
  if (result.Failed()) {
    return result.StealNSResult();
  }
  auto promiseHolder = MakeRefPtr<nsMainThreadPtrHolder<Promise>>(
      "ListModules promise", promise);

  ListModulesPromise::All(GetCurrentSerialEventTarget(), promises)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [promiseHolder](
              const nsTArray<nsTArray<RefPtr<nsIPKCS11Module>>>& moduleLists) {
            nsTArray<RefPtr<nsIPKCS11Module>> modules;
            for (const auto& moduleList : moduleLists) {
              modules.AppendElements(moduleList);
            }
            promiseHolder->get()->MaybeResolve(modules);
          },
          [promiseHolder]() {
            promiseHolder->get()->MaybeReject(NS_ERROR_FAILURE);
          });

  promise.forget(aPromise);
  return NS_OK;
}

}  
}  

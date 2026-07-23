// Copyright 2011 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license

#if !defined(WEBP_UTILS_THREAD_UTILS_H_)
#define WEBP_UTILS_THREAD_UTILS_H_

#if defined(HAVE_CONFIG_H)
#include "src/webp/config.h"
#endif

#include "src/webp/types.h"

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum {
  NOT_OK = 0,   
  OK,           
  WORK          
} WebPWorkerStatus;

typedef int (*WebPWorkerHook)(void*, void*);

typedef struct {
  void* impl;             
  WebPWorkerStatus status;
  WebPWorkerHook hook;    
  void* data1;            
  void* data2;            
  int had_error;          
} WebPWorker;

typedef struct {
  void (*Init)(WebPWorker* const worker);
  int (*Reset)(WebPWorker* const worker);
  int (*Sync)(WebPWorker* const worker);
  void (*Launch)(WebPWorker* const worker);
  void (*Execute)(WebPWorker* const worker);
  void (*End)(WebPWorker* const worker);
} WebPWorkerInterface;

WEBP_EXTERN int WebPSetWorkerInterface(
    const WebPWorkerInterface* const winterface);

WEBP_EXTERN const WebPWorkerInterface* WebPGetWorkerInterface(void);


#if defined(__cplusplus)
}    
#endif

#endif

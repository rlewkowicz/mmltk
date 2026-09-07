/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#if !defined(AOM_AOM_UTIL_AOM_THREAD_H_)
#define AOM_AOM_UTIL_AOM_THREAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

typedef enum {
  AVX_WORKER_STATUS_NOT_OK = 0,  
  AVX_WORKER_STATUS_OK,          
  AVX_WORKER_STATUS_WORKING      
} AVxWorkerStatus;

typedef int (*AVxWorkerHook)(void *, void *);

typedef struct AVxWorkerImpl AVxWorkerImpl;

typedef struct {
  AVxWorkerImpl *impl_;
  AVxWorkerStatus status_;
  const char *thread_name;
  AVxWorkerHook hook;  
  void *data1;         
  void *data2;         
  int had_error;       
} AVxWorker;

typedef struct {
  void (*init)(AVxWorker *const worker);
  int (*reset)(AVxWorker *const worker);
  int (*sync)(AVxWorker *const worker);
  void (*launch)(AVxWorker *const worker);
  void (*execute)(AVxWorker *const worker);
  void (*end)(AVxWorker *const worker);
} AVxWorkerInterface;

int aom_set_worker_interface(const AVxWorkerInterface *const winterface);

const AVxWorkerInterface *aom_get_worker_interface(void);


#if defined(__cplusplus)
}  
#endif

#endif

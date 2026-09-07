// Copyright 2011 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license

#include <assert.h>
#include <string.h>   // for memset()

#include "src/utils/thread_utils.h"
#include "src/utils/utils.h"

#if defined(WEBP_USE_THREAD)


#include <pthread.h>


typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t  condition;
  pthread_t       thread;
} WebPWorkerImpl;

# define THREADFN void*
# define THREAD_RETURN(val) val


static THREADFN ThreadLoop(void* ptr) {
  WebPWorker* const worker = (WebPWorker*)ptr;
  WebPWorkerImpl* const impl = (WebPWorkerImpl*)worker->impl;
  int done = 0;
  while (!done) {
    pthread_mutex_lock(&impl->mutex);
    while (worker->status == OK) {   
      pthread_cond_wait(&impl->condition, &impl->mutex);
    }
    if (worker->status == WORK) {
      WebPGetWorkerInterface()->Execute(worker);
      worker->status = OK;
    } else if (worker->status == NOT_OK) {   
      done = 1;
    }
    pthread_mutex_unlock(&impl->mutex);
    pthread_cond_signal(&impl->condition);
  }
  return THREAD_RETURN(NULL);    
}

static void ChangeState(WebPWorker* const worker, WebPWorkerStatus new_status) {
  WebPWorkerImpl* const impl = (WebPWorkerImpl*)worker->impl;
  if (impl == NULL) return;

  pthread_mutex_lock(&impl->mutex);
  if (worker->status >= OK) {
    while (worker->status != OK) {
      pthread_cond_wait(&impl->condition, &impl->mutex);
    }
    if (new_status != OK) {
      worker->status = new_status;
      pthread_mutex_unlock(&impl->mutex);
      pthread_cond_signal(&impl->condition);
      return;
    }
  }
  pthread_mutex_unlock(&impl->mutex);
}

#endif


static void Init(WebPWorker* const worker) {
  memset(worker, 0, sizeof(*worker));
  worker->status = NOT_OK;
}

static int Sync(WebPWorker* const worker) {
#if defined(WEBP_USE_THREAD)
  ChangeState(worker, OK);
#endif
  assert(worker->status <= OK);
  return !worker->had_error;
}

static int Reset(WebPWorker* const worker) {
  int ok = 1;
  worker->had_error = 0;
  if (worker->status < OK) {
#if defined(WEBP_USE_THREAD)
    WebPWorkerImpl* const impl =
        (WebPWorkerImpl*)WebPSafeCalloc(1, sizeof(WebPWorkerImpl));
    worker->impl = (void*)impl;
    if (worker->impl == NULL) {
      return 0;
    }
    if (pthread_mutex_init(&impl->mutex, NULL)) {
      goto Error;
    }
    if (pthread_cond_init(&impl->condition, NULL)) {
      pthread_mutex_destroy(&impl->mutex);
      goto Error;
    }
    pthread_mutex_lock(&impl->mutex);
    ok = !pthread_create(&impl->thread, NULL, ThreadLoop, worker);
    if (ok) worker->status = OK;
    pthread_mutex_unlock(&impl->mutex);
    if (!ok) {
      pthread_mutex_destroy(&impl->mutex);
      pthread_cond_destroy(&impl->condition);
 Error:
      WebPSafeFree(impl);
      worker->impl = NULL;
      return 0;
    }
#else
    worker->status = OK;
#endif
  } else if (worker->status > OK) {
    ok = Sync(worker);
  }
  assert(!ok || (worker->status == OK));
  return ok;
}

static void Execute(WebPWorker* const worker) {
  if (worker->hook != NULL) {
    worker->had_error |= !worker->hook(worker->data1, worker->data2);
  }
}

static void Launch(WebPWorker* const worker) {
#if defined(WEBP_USE_THREAD)
  ChangeState(worker, WORK);
#else
  Execute(worker);
#endif
}

static void End(WebPWorker* const worker) {
#if defined(WEBP_USE_THREAD)
  if (worker->impl != NULL) {
    WebPWorkerImpl* const impl = (WebPWorkerImpl*)worker->impl;
    ChangeState(worker, NOT_OK);
    pthread_join(impl->thread, NULL);
    pthread_mutex_destroy(&impl->mutex);
    pthread_cond_destroy(&impl->condition);
    WebPSafeFree(impl);
    worker->impl = NULL;
  }
#else
  worker->status = NOT_OK;
  assert(worker->impl == NULL);
#endif
  assert(worker->status == NOT_OK);
}


static WebPWorkerInterface g_worker_interface = {
  Init, Reset, Sync, Launch, Execute, End
};

int WebPSetWorkerInterface(const WebPWorkerInterface* const winterface) {
  if (winterface == NULL ||
      winterface->Init == NULL || winterface->Reset == NULL ||
      winterface->Sync == NULL || winterface->Launch == NULL ||
      winterface->Execute == NULL || winterface->End == NULL) {
    return 0;
  }
  g_worker_interface = *winterface;
  return 1;
}

const WebPWorkerInterface* WebPGetWorkerInterface(void) {
  return &g_worker_interface;
}


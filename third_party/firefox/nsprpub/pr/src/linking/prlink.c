/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "primpl.h"

#include <string.h>

#if defined(XP_UNIX)
#if defined(USE_DLFCN)
#    include <dlfcn.h>
#if !defined(RTLD_NOW)
#      define RTLD_NOW 0
#endif
#if !defined(RTLD_LAZY)
#      define RTLD_LAZY RTLD_NOW
#endif
#if !defined(RTLD_GLOBAL)
#      define RTLD_GLOBAL 0
#endif
#if !defined(RTLD_LOCAL)
#      define RTLD_LOCAL 0
#endif
#elif defined(USE_HPSHL)
#    include <dl.h>
#endif
#endif

#define _PR_DEFAULT_LD_FLAGS PR_LD_LAZY

#if ((defined(OPENBSD) || defined(NETBSD)) && !defined(__ELF__))
#  define NEED_LEADING_UNDERSCORE
#endif

#define PR_LD_PATHW 0x8000 /* for PR_LibSpec_PathnameU */


struct PRLibrary {
  char* name; 
  PRLibrary* next;
  int refCount;
  const PRStaticLinkTable* staticTable;

#if defined(XP_PC)
  HINSTANCE dlh;
#endif

#if defined(XP_UNIX)
#if defined(USE_HPSHL)
  shl_t dlh;
#else
  void* dlh;
#endif
#endif
};

static PRLibrary* pr_loadmap;
static PRLibrary* pr_exe_loadmap;
static PRMonitor* pr_linker_lock;
static char* _pr_currentLibPath = NULL;

static PRLibrary* pr_LoadLibraryByPathname(const char* name, PRIntn flags);


#if !defined(USE_DLFCN) && !defined(HAVE_STRERROR)
#  define ERR_STR_BUF_LENGTH 20
#endif

static void DLLErrorInternal(PRIntn oserr)
{
  const char* error = NULL;
#if defined(USE_DLFCN)
  error = dlerror(); 
#elif defined(HAVE_STRERROR)
  error = strerror(oserr); 
#else
  char errStrBuf[ERR_STR_BUF_LENGTH];
  PR_snprintf(errStrBuf, sizeof(errStrBuf), "error %d", oserr);
  error = errStrBuf;
#endif
  if (NULL != error) {
    PR_SetErrorText(strlen(error), error);
  }
} 

void _PR_InitLinker(void) {
  PRLibrary* lm = NULL;
#if defined(XP_UNIX)
  void* h;
#endif

  if (!pr_linker_lock) {
    pr_linker_lock = PR_NewNamedMonitor("linker-lock");
  }
  PR_EnterMonitor(pr_linker_lock);

#if defined(XP_PC)
  lm = PR_NEWZAP(PRLibrary);
  lm->name = strdup("Executable");
  lm->dlh = GetModuleHandle(NULL);

  lm->refCount = 1;
  lm->staticTable = NULL;
  pr_exe_loadmap = lm;
  pr_loadmap = lm;

#elif defined(XP_UNIX)
#if defined(HAVE_DLL)
#if defined(USE_DLFCN) && !defined(NO_DLOPEN_NULL)
  h = dlopen(0, RTLD_LAZY);
  if (!h) {
    char* error;

    DLLErrorInternal(_MD_ERRNO());
    error = (char*)PR_MALLOC(PR_GetErrorTextLength());
    (void)PR_GetErrorText(error);
    fprintf(stderr, "failed to initialize shared libraries [%s]\n", error);
    PR_DELETE(error);
    abort(); 
  }
#elif defined(USE_HPSHL)
  h = NULL;
#elif defined(NO_DLOPEN_NULL)
  h = NULL;  
#else
#      error no dll strategy
#endif

  lm = PR_NEWZAP(PRLibrary);
  if (lm) {
    lm->name = strdup("a.out");
    lm->refCount = 1;
    lm->dlh = h;
    lm->staticTable = NULL;
  }
  pr_exe_loadmap = lm;
  pr_loadmap = lm;
#endif
#endif

  if (lm) {
    PR_LOG(_pr_linker_lm, PR_LOG_MIN, ("Loaded library %s (init)", lm->name));
  }

  PR_ExitMonitor(pr_linker_lock);
}

void _PR_ShutdownLinker(void) {

  PR_DestroyMonitor(pr_linker_lock);
  pr_linker_lock = NULL;

  if (_pr_currentLibPath) {
    free(_pr_currentLibPath);
    _pr_currentLibPath = NULL;
  }
}


PR_IMPLEMENT(PRStatus) PR_SetLibraryPath(const char* path) {
  PRStatus rv = PR_SUCCESS;

  if (!_pr_initialized) {
    _PR_ImplicitInitialization();
  }
  PR_EnterMonitor(pr_linker_lock);
  if (_pr_currentLibPath) {
    free(_pr_currentLibPath);
  }
  if (path) {
    _pr_currentLibPath = strdup(path);
    if (!_pr_currentLibPath) {
      PR_SetError(PR_OUT_OF_MEMORY_ERROR, 0);
      rv = PR_FAILURE;
    }
  } else {
    _pr_currentLibPath = 0;
  }
  PR_ExitMonitor(pr_linker_lock);
  return rv;
}

PR_IMPLEMENT(char*)
PR_GetLibraryPath(void) {
  char* ev;
  char* copy = NULL; 

  if (!_pr_initialized) {
    _PR_ImplicitInitialization();
  }
  PR_EnterMonitor(pr_linker_lock);
  if (_pr_currentLibPath != NULL) {
    goto exit;
  }


#if defined(XP_PC)
  ev = getenv("LD_LIBRARY_PATH");
  if (!ev) {
    ev = ".;\\lib";
  }
  ev = strdup(ev);
#endif

#if defined(XP_UNIX)
#if defined(USE_DLFCN)
  {
    char* p = NULL;
    int len;

    ev = getenv("LD_LIBRARY_PATH");
    if (!ev) {
      ev = "/usr/lib:/lib";
    }
    len = strlen(ev) + 1; 

    p = (char*)malloc(len);
    if (p) {
      strcpy(p, ev);
    } 
    ev = p;
    PR_LOG(_pr_io_lm, PR_LOG_NOTICE, ("linker path '%s'", ev));
  }
#else
  ev = strdup("");
#endif
#endif

  _pr_currentLibPath = ev;

exit:
  if (_pr_currentLibPath) {
    copy = strdup(_pr_currentLibPath);
  }
  PR_ExitMonitor(pr_linker_lock);
  if (!copy) {
    PR_SetError(PR_OUT_OF_MEMORY_ERROR, 0);
  }
  return copy;
}

PR_IMPLEMENT(char*)
PR_GetLibraryName(const char* path, const char* lib) {
  char* fullname;

#if defined(XP_PC)
  if (strstr(lib, PR_DLL_SUFFIX) == NULL) {
    if (path) {
      fullname = PR_smprintf("%s\\%s%s", path, lib, PR_DLL_SUFFIX);
    } else {
      fullname = PR_smprintf("%s%s", lib, PR_DLL_SUFFIX);
    }
  } else {
    if (path) {
      fullname = PR_smprintf("%s\\%s", path, lib);
    } else {
      fullname = PR_smprintf("%s", lib);
    }
  }
#endif
#if defined(XP_UNIX)
  if (strstr(lib, PR_DLL_SUFFIX) == NULL) {
    if (path) {
      fullname = PR_smprintf("%s/lib%s%s", path, lib, PR_DLL_SUFFIX);
    } else {
      fullname = PR_smprintf("lib%s%s", lib, PR_DLL_SUFFIX);
    }
  } else {
    if (path) {
      fullname = PR_smprintf("%s/%s", path, lib);
    } else {
      fullname = PR_smprintf("%s", lib);
    }
  }
#endif
  return fullname;
}

PR_IMPLEMENT(void)
PR_FreeLibraryName(char* mem) { PR_smprintf_free(mem); }

static PRLibrary* pr_UnlockedFindLibrary(const char* name) {
  PRLibrary* lm = pr_loadmap;
  const char* np = strrchr(name, PR_DIRECTORY_SEPARATOR);
  np = np ? np + 1 : name;
  while (lm) {
    const char* cp = strrchr(lm->name, PR_DIRECTORY_SEPARATOR);
    cp = cp ? cp + 1 : lm->name;
    if (strcmp(np, cp) == 0)
    {
      lm->refCount++;
      PR_LOG(_pr_linker_lm, PR_LOG_MIN,
             ("%s incr => %d (find lib)", lm->name, lm->refCount));
      return lm;
    }
    lm = lm->next;
  }
  return NULL;
}

PR_IMPLEMENT(PRLibrary*)
PR_LoadLibraryWithFlags(PRLibSpec libSpec, PRIntn flags) {
  if (flags == 0) {
    flags = _PR_DEFAULT_LD_FLAGS;
  }
  switch (libSpec.type) {
    case PR_LibSpec_Pathname:
      return pr_LoadLibraryByPathname(libSpec.value.pathname, flags);
    default:
      PR_SetError(PR_INVALID_ARGUMENT_ERROR, 0);
      return NULL;
  }
}

PR_IMPLEMENT(PRLibrary*)
PR_LoadLibrary(const char* name) {
  PRLibSpec libSpec;

  libSpec.type = PR_LibSpec_Pathname;
  libSpec.value.pathname = name;
  return PR_LoadLibraryWithFlags(libSpec, 0);
}

static PRLibrary* pr_LoadLibraryByPathname(const char* name, PRIntn flags) {
  PRLibrary* lm;
  PRLibrary* result = NULL;
  PRInt32 oserr;

  if (!_pr_initialized) {
    _PR_ImplicitInitialization();
  }

  PR_EnterMonitor(pr_linker_lock);

  result = pr_UnlockedFindLibrary(name);

  if (result != NULL) {
    goto unlock;
  }

  lm = PR_NEWZAP(PRLibrary);
  if (lm == NULL) {
    oserr = _MD_ERRNO();
    goto unlock;
  }
  lm->staticTable = NULL;


#if defined(XP_UNIX)
#if defined(HAVE_DLL)
  {
#if defined(USE_DLFCN)
#if defined(NTO)
    int dl_flags = RTLD_GROUP;
#else
    int dl_flags = 0;
#endif
    void* h = NULL;

    if (flags & PR_LD_LAZY) {
      dl_flags |= RTLD_LAZY;
    }
    if (flags & PR_LD_NOW) {
      dl_flags |= RTLD_NOW;
    }
    if (flags & PR_LD_GLOBAL) {
      dl_flags |= RTLD_GLOBAL;
    }
    if (flags & PR_LD_LOCAL) {
      dl_flags |= RTLD_LOCAL;
    }
    h = dlopen(name, dl_flags);
#elif defined(USE_HPSHL)
    int shl_flags = 0;
    shl_t h;

    if (strchr(name, PR_DIRECTORY_SEPARATOR) == NULL) {
      shl_flags |= DYNAMIC_PATH;
    }
    if (flags & PR_LD_LAZY) {
      shl_flags |= BIND_DEFERRED;
    }
    if (flags & PR_LD_NOW) {
      shl_flags |= BIND_IMMEDIATE;
    }
    h = shl_load(name, shl_flags, 0L);
#else
#      error Configuration error
#endif
    if (!h) {
      oserr = _MD_ERRNO();
      PR_DELETE(lm);
      goto unlock;
    }
    lm->name = strdup(name);
    lm->dlh = h;
    lm->next = pr_loadmap;
    pr_loadmap = lm;
  }
#endif
#endif

  lm->refCount = 1;

  result = lm; 
  PR_LOG(_pr_linker_lm, PR_LOG_MIN, ("Loaded library %s (load lib)", lm->name));

unlock:
  if (result == NULL) {
    PR_SetError(PR_LOAD_LIBRARY_ERROR, oserr);
    DLLErrorInternal(oserr); 
  }
  PR_ExitMonitor(pr_linker_lock);
  return result;
}

PR_IMPLEMENT(PRStatus)
PR_UnloadLibrary(PRLibrary* lib) {
  int result = 0;
  PRStatus status = PR_SUCCESS;

  if (lib == 0) {
    PR_SetError(PR_INVALID_ARGUMENT_ERROR, 0);
    return PR_FAILURE;
  }

  PR_EnterMonitor(pr_linker_lock);

  if (lib->refCount <= 0) {
    PR_ExitMonitor(pr_linker_lock);
    PR_SetError(PR_INVALID_ARGUMENT_ERROR, 0);
    return PR_FAILURE;
  }

  if (--lib->refCount > 0) {
    PR_LOG(_pr_linker_lm, PR_LOG_MIN,
           ("%s decr => %d", lib->name, lib->refCount));
    goto done;
  }

#if defined(XP_UNIX)
#if defined(HAVE_DLL)
#if defined(USE_DLFCN)
  result = dlclose(lib->dlh);
#elif defined(USE_HPSHL)
  result = shl_unload(lib->dlh);
#else
#      error Configuration error
#endif
#endif
#endif
#if defined(XP_PC)
  if (lib->dlh) {
    FreeLibrary((HINSTANCE)(lib->dlh));
    lib->dlh = (HINSTANCE)NULL;
  }
#endif

  if (pr_loadmap == lib) {
    pr_loadmap = pr_loadmap->next;
  } else if (pr_loadmap != NULL) {
    PRLibrary* prev = pr_loadmap;
    PRLibrary* next = pr_loadmap->next;
    while (next != NULL) {
      if (next == lib) {
        prev->next = next->next;
        goto freeLib;
      }
      prev = next;
      next = next->next;
    }
    PR_NOT_REACHED("_pr_loadmap and lib->refCount inconsistent");
    if (result == 0) {
      PR_SetError(PR_INVALID_ARGUMENT_ERROR, 0);
      status = PR_FAILURE;
    }
  }

freeLib:
  PR_LOG(_pr_linker_lm, PR_LOG_MIN, ("Unloaded library %s", lib->name));
  free(lib->name);
  lib->name = NULL;
  PR_DELETE(lib);
  if (result != 0) {
    PR_SetError(PR_UNLOAD_LIBRARY_ERROR, _MD_ERRNO());
    DLLErrorInternal(_MD_ERRNO());
    status = PR_FAILURE;
  }

done:
  PR_ExitMonitor(pr_linker_lock);
  return status;
}

static void* pr_FindSymbolInLib(PRLibrary* lm, const char* name) {
  void* f = NULL;

  if (lm->staticTable != NULL) {
    const PRStaticLinkTable* tp;
    for (tp = lm->staticTable; tp->name; tp++) {
      if (strcmp(name, tp->name) == 0) {
        return (void*)tp->fp;
      }
    }
    PR_SetError(PR_FIND_SYMBOL_ERROR, 0);
    return (void*)NULL;
  }


#if defined(XP_UNIX)
#if defined(HAVE_DLL)
#if defined(USE_DLFCN)
  f = dlsym(lm->dlh, name);
#elif defined(USE_HPSHL)
  if (shl_findsym(&lm->dlh, name, TYPE_PROCEDURE, &f) == -1) {
    f = NULL;
  }
#endif
#endif
#endif
  if (f == NULL) {
    PR_SetError(PR_FIND_SYMBOL_ERROR, _MD_ERRNO());
    DLLErrorInternal(_MD_ERRNO());
  }
  return f;
}

PR_IMPLEMENT(void*)
PR_FindSymbol(PRLibrary* lib, const char* raw_name) {
  void* f = NULL;
#if defined(NEED_LEADING_UNDERSCORE)
  char* name;
#else
  const char* name;
#endif
#if defined(NEED_LEADING_UNDERSCORE)
  name = PR_smprintf("_%s", raw_name);
#else
  name = raw_name;
#endif

  PR_EnterMonitor(pr_linker_lock);
  PR_ASSERT(lib != NULL);
  f = pr_FindSymbolInLib(lib, name);

#if defined(NEED_LEADING_UNDERSCORE)
  PR_smprintf_free(name);
#endif

  PR_ExitMonitor(pr_linker_lock);
  return f;
}

PR_IMPLEMENT(PRFuncPtr)
PR_FindFunctionSymbol(PRLibrary* lib, const char* raw_name) {
  return ((PRFuncPtr)PR_FindSymbol(lib, raw_name));
}

PR_IMPLEMENT(void*)
PR_FindSymbolAndLibrary(const char* raw_name, PRLibrary** lib) {
  void* f = NULL;
#if defined(NEED_LEADING_UNDERSCORE)
  char* name;
#else
  const char* name;
#endif
  PRLibrary* lm;

  if (!_pr_initialized) {
    _PR_ImplicitInitialization();
  }
#if defined(NEED_LEADING_UNDERSCORE)
  name = PR_smprintf("_%s", raw_name);
#else
  name = raw_name;
#endif

  PR_EnterMonitor(pr_linker_lock);

  for (lm = pr_loadmap; lm != NULL; lm = lm->next) {
    f = pr_FindSymbolInLib(lm, name);
    if (f != NULL) {
      *lib = lm;
      lm->refCount++;
      PR_LOG(_pr_linker_lm, PR_LOG_MIN,
             ("%s incr => %d (for %s)", lm->name, lm->refCount, name));
      break;
    }
  }
#if defined(NEED_LEADING_UNDERSCORE)
  PR_smprintf_free(name);
#endif

  PR_ExitMonitor(pr_linker_lock);
  return f;
}

PR_IMPLEMENT(PRFuncPtr)
PR_FindFunctionSymbolAndLibrary(const char* raw_name, PRLibrary** lib) {
  return ((PRFuncPtr)PR_FindSymbolAndLibrary(raw_name, lib));
}

PR_IMPLEMENT(PRLibrary*)
PR_LoadStaticLibrary(const char* name, const PRStaticLinkTable* slt) {
  PRLibrary* lm = NULL;
  PRLibrary* result = NULL;

  if (!_pr_initialized) {
    _PR_ImplicitInitialization();
  }

  PR_EnterMonitor(pr_linker_lock);

  result = pr_UnlockedFindLibrary(name);
  if (result != NULL) {
    PR_ASSERT((result->staticTable == NULL) || (result->staticTable == slt));
    result->staticTable = slt;
    goto unlock;
  }

  lm = PR_NEWZAP(PRLibrary);
  if (lm == NULL) {
    goto unlock;
  }

  lm->name = strdup(name);
  lm->refCount = 1;
  lm->dlh = pr_exe_loadmap ? pr_exe_loadmap->dlh : 0;
  lm->staticTable = slt;
  lm->next = pr_loadmap;
  pr_loadmap = lm;

  result = lm; 
  PR_ASSERT(lm->refCount == 1);
  PR_LOG(_pr_linker_lm, PR_LOG_MIN,
         ("Loaded library %s (static lib)", lm->name));
unlock:
  PR_ExitMonitor(pr_linker_lock);
  return result;
}

PR_IMPLEMENT(char*)
PR_GetLibraryFilePathname(const char* name, PRFuncPtr addr) {
#if defined(USE_DLFCN) && defined(HAVE_DLADDR)
  Dl_info dli;
  char* result;

  if (dladdr((void*)addr, &dli) == 0) {
    PR_SetError(PR_LIBRARY_NOT_LOADED_ERROR, _MD_ERRNO());
    DLLErrorInternal(_MD_ERRNO());
    return NULL;
  }
  result = PR_Malloc(strlen(dli.dli_fname) + 1);
  if (result != NULL) {
    strcpy(result, dli.dli_fname);
  }
  return result;
#else
  PR_SetError(PR_NOT_IMPLEMENTED_ERROR, 0);
  return NULL;
#endif
}

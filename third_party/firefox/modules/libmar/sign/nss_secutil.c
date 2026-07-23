/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "nss_secutil.h"

#include "prprf.h"
#  include <unistd.h>


char* GetPasswordString(void* arg, char* prompt) {
  FILE* input = stdin;
  char phrase[200] = {'\0'};
  int isInputTerminal = isatty(fileno(stdin));

  if (isInputTerminal) {
    static char consoleName[] = {
#if defined(XP_UNIX)
        "/dev/tty"
#else
        "CON:"
#endif
    };

    input = fopen(consoleName, "r");
    if (input == NULL) {
      fprintf(stderr, "Error opening input terminal for read\n");
      return NULL;
    }
  }

  if (isInputTerminal) {
    fprintf(stdout, "Please enter your password:\n");
    fflush(stdout);
  }

  if (!QUIET_FGETS(phrase, sizeof(phrase), input)) {
    fprintf(stderr, "QUIET_FGETS failed\n");
    if (isInputTerminal) {
      fclose(input);
    }
    return NULL;
  }

  if (isInputTerminal) {
    fprintf(stdout, "\n");
  }

  if (isInputTerminal) {
    fclose(input);
  }

  if (phrase[PORT_Strlen(phrase) - 1] == '\n' ||
      phrase[PORT_Strlen(phrase) - 1] == '\r') {
    phrase[PORT_Strlen(phrase) - 1] = 0;
  }
  return (char*)PORT_Strdup(phrase);
}

char* SECU_FilePasswd(PK11SlotInfo* slot, PRBool retry, void* arg) {
  char *phrases, *phrase;
  PRFileDesc* fd;
  int32_t nb;
  char* pwFile = arg;
  int i;
  const long maxPwdFileSize = 4096;
  char* tokenName = NULL;
  int tokenLen = 0;

  if (!pwFile) return 0;

  if (retry) {
    return 0; 
  }

  phrases = PORT_ZAlloc(maxPwdFileSize);

  if (!phrases) {
    return 0; 
  }

  fd = PR_Open(pwFile, PR_RDONLY, 0);
  if (!fd) {
    fprintf(stderr, "No password file \"%s\" exists.\n", pwFile);
    PORT_Free(phrases);
    return NULL;
  }

  nb = PR_Read(fd, phrases, maxPwdFileSize);

  PR_Close(fd);

  if (nb == 0) {
    fprintf(stderr, "password file contains no data\n");
    PORT_Free(phrases);
    return NULL;
  }

  if (slot) {
    tokenName = PK11_GetTokenName(slot);
    if (tokenName) {
      tokenLen = PORT_Strlen(tokenName);
    }
  }
  i = 0;
  do {
    int startphrase = i;
    int phraseLen;

    while (phrases[i] != '\r' && phrases[i] != '\n' && i < nb) i++;
    if (i < nb) {
      phrases[i++] = '\0';
    }
    while ((i < nb) && (phrases[i] == '\r' || phrases[i] == '\n')) {
      phrases[i++] = '\0';
    }
    phrase = &phrases[startphrase];
    if (!tokenName) break;
    if (PORT_Strncmp(phrase, tokenName, tokenLen)) continue;
    phraseLen = PORT_Strlen(phrase);
    if (phraseLen < (tokenLen + 1)) continue;
    if (phrase[tokenLen] != ':') continue;
    phrase = &phrase[tokenLen + 1];
    break;

  } while (i < nb);

  phrase = PORT_Strdup((char*)phrase);
  PORT_Free(phrases);
  return phrase;
}

char* SECU_GetModulePassword(PK11SlotInfo* slot, PRBool retry, void* arg) {
  char prompt[255];
  secuPWData* pwdata = (secuPWData*)arg;
  secuPWData pwnull = {PW_NONE, 0};
  secuPWData pwxtrn = {PW_EXTERNAL, "external"};
  char* pw;

  if (pwdata == NULL) pwdata = &pwnull;

  if (PK11_ProtectedAuthenticationPath(slot)) {
    pwdata = &pwxtrn;
  }
  if (retry && pwdata->source != PW_NONE) {
    PR_fprintf(PR_STDERR, "Incorrect password/PIN entered.\n");
    return NULL;
  }

  switch (pwdata->source) {
    case PW_NONE:
      sprintf(prompt,
              "Enter Password or Pin for \"%s\":", PK11_GetTokenName(slot));
      return GetPasswordString(NULL, prompt);
    case PW_FROMFILE:
      pw = SECU_FilePasswd(slot, retry, pwdata->data);
      pwdata->source = PW_PLAINTEXT;
      pwdata->data = strdup(pw);
      return pw;
    case PW_EXTERNAL:
      sprintf(prompt,
              "Press Enter, then enter PIN for \"%s\" on external device.\n",
              PK11_GetTokenName(slot));
      pw = GetPasswordString(NULL, prompt);
      if (pw) {
        memset(pw, 0, PORT_Strlen(pw));
        PORT_Free(pw);
      }
    case PW_PLAINTEXT:
      return strdup(pwdata->data);
    default:
      break;
  }

  PR_fprintf(PR_STDERR, "Password check failed:  No password found.\n");
  return NULL;
}

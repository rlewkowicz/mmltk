/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "secutil.h"


#if defined(XP_UNIX)
#include <termios.h>
#include <unistd.h> /* for isatty() */
#endif

#define QUIET_FGETS fgets

static void
echoOff(int fd)
{
    if (isatty(fd)) {
        struct termios tio;
        tcgetattr(fd, &tio);
        tio.c_lflag &= ~ECHO;
        tcsetattr(fd, TCSAFLUSH, &tio);
    }
}

static void
echoOn(int fd)
{
    if (isatty(fd)) {
        struct termios tio;
        tcgetattr(fd, &tio);
        tio.c_lflag |= ECHO;
        tcsetattr(fd, TCSAFLUSH, &tio);
    }
}

char *
SEC_GetPassword(FILE *input, FILE *output, char *prompt,
                PRBool (*ok)(char *))
{
    int infd = fileno(input);
    int isTTY = isatty(infd);
    char phrase[500] = { '\0' }; 

    for (;;) {
        if (isTTY) {
            fprintf(output, "%s", prompt);
            fflush(output);
            echoOff(infd);
        }

        if (QUIET_FGETS(phrase, sizeof(phrase), input) == NULL) {
            return NULL;
        }

        if (isTTY) {
            fprintf(output, "\n");
            echoOn(infd);
        }

        phrase[PORT_Strlen(phrase) - 1] = 0;

        if (!(*ok)(phrase)) {
            if (!isTTY)
                return NULL;
            fprintf(output, "Password must be at least 8 characters long with one or more\n");
            fprintf(output, "non-alphabetic characters\n");
            continue;
        }
        return (char *)PORT_Strdup(phrase);
    }
}

PRBool
SEC_CheckPassword(char *cp)
{
    int len;
    char *end;

    len = PORT_Strlen(cp);
    if (len < 8) {
        return PR_FALSE;
    }
    end = cp + len;
    while (cp < end) {
        unsigned char ch = *cp++;
        if (!((ch >= 'A') && (ch <= 'Z')) &&
            !((ch >= 'a') && (ch <= 'z'))) {
            return PR_TRUE;
        }
    }
    return PR_FALSE;
}

PRBool
SEC_BlindCheckPassword(char *cp)
{
    if (cp != NULL) {
        return PR_TRUE;
    }
    return PR_FALSE;
}


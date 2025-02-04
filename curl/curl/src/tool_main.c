/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2014, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/
#include "tool_setup.h"

#include <sys/stat.h>

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif

#ifdef USE_NSS
#include <nspr.h>
#include <plarenas.h>
#endif

#define ENABLE_CURLX_PRINTF
/* use our own printf() functions */
#include "curlx.h"

#include "tool_cfgable.h"
#include "tool_convert.h"
#include "tool_msgs.h"
#include "tool_operate.h"
#include "tool_panykey.h"
#include "tool_vms.h"
#include "tool_main.h"
#include "tool_libinfo.h"
#include "ios_error.h"

/*
 * This is low-level hard-hacking memory leak tracking and similar. Using
 * the library level code from this client-side is ugly, but we do this
 * anyway for convenience.
 */
#include "memdebug.h" /* keep this as LAST include */

#ifdef __VMS
/*
 * vms_show is a global variable, used in main() as parameter for
 * function vms_special_exit() to allow proper curl tool exiting.
 * Its value may be set in other tool_*.c source files thanks to
 * forward declaration present in tool_vms.h
 */
int vms_show = 0;
#endif

/* if we build a static library for unit tests, there is no main() function */
#ifndef UNITTESTS

/*
 * Ensure that file descriptors 0, 1 and 2 (stdin, stdout, stderr) are
 * open before starting to run.  Otherwise, the first three network
 * sockets opened by curl could be used for input sources, downloaded data
 * or error logs as they will effectively be stdin, stdout and/or stderr.
 */
static void main_checkfds(void)
{
#ifdef HAVE_PIPE
  int fd[2] = { fileno(thread_stdin), fileno(thread_stdin) };
  while(fd[0] == fileno(thread_stdin) ||
        fd[0] == fileno(thread_stdout) ||
        fd[0] == fileno(thread_stderr) ||
        fd[1] == fileno(thread_stdin) ||
        fd[1] == fileno(thread_stdout) ||
        fd[1] == fileno(thread_stderr))
    if(pipe(fd) < 0)
      return;   /* Out of handles. This isn't really a big problem now, but
                   will be when we try to create a socket later. */
  close(fd[0]);
  close(fd[1]);
#endif
}

#ifdef CURLDEBUG
static void memory_tracking_init(void)
{
  char *env;
  /* if CURL_MEMDEBUG is set, this starts memory tracking message logging */
  env = curlx_getenv("CURL_MEMDEBUG");
  if(env) {
    /* use the value as file name */
    char fname[CURL_MT_LOGFNAME_BUFSIZE];
    if(strlen(env) >= CURL_MT_LOGFNAME_BUFSIZE)
      env[CURL_MT_LOGFNAME_BUFSIZE-1] = '\0';
    strcpy(fname, env);
    curl_free(env);
    curl_memdebug(fname);
    /* this weird stuff here is to make curl_free() get called
       before curl_memdebug() as otherwise memory tracking will
       log a free() without an alloc! */
  }
  /* if CURL_MEMLIMIT is set, this enables fail-on-alloc-number-N feature */
  env = curlx_getenv("CURL_MEMLIMIT");
  if(env) {
    char *endptr;
    long num = strtol(env, &endptr, 10);
    if((endptr != env) && (endptr == env + strlen(env)) && (num > 0))
      curl_memlimit(num);
    curl_free(env);
  }
}
#else
#  define memory_tracking_init() Curl_nop_stmt
#endif

/*
 * This is the main global constructor for the app. Call this before
 * _any_ libcurl usage. If this fails, *NO* libcurl functions may be
 * used, or havoc may be the result.
 */
static CURLcode main_init(struct GlobalConfig *config)
{
  CURLcode result = CURLE_OK;

#if defined(__DJGPP__) || defined(__GO32__)
  /* stop stat() wasting time */
  _djstat_flags |= _STAT_INODE | _STAT_EXEC_MAGIC | _STAT_DIRSIZE;
#endif

  /* Initialise the global config */
  config->showerror = -1;             /* Will show errors */
  config->errors = thread_stderr;            /* Default errors to stderr */

  /* Allocate the initial operate config */
  config->first = config->last = malloc(sizeof(struct OperationConfig));
  if(config->first) {
    /* Perform the libcurl initialization */
    result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if(!result) {
      /* Get information about libcurl */
      result = get_libcurl_info();

      if(!result) {
        /* Get a curl handle to use for all forthcoming curl transfers */
        config->easy = curl_easy_init();
        if(config->easy) {
          /* Initialise the config */
          config_init(config->first);
          config->first->easy = config->easy;
          config->first->global = config;
        }
        else {
          helpf(thread_stderr, "error initializing curl easy handle\n");
          result = CURLE_FAILED_INIT;
          free(config->first);
        }
      }
      else {
        helpf(thread_stderr, "error retrieving curl library information\n");
        free(config->first);
      }
    }
    else {
      helpf(thread_stderr, "error initializing curl library\n");
      free(config->first);
    }
  }
  else {
    helpf(thread_stderr, "error initializing curl\n");
    result = CURLE_FAILED_INIT;
  }

  return result;
}

static void free_config_fields(struct GlobalConfig *config)
{
  Curl_safefree(config->trace_dump);

  if(config->errors_fopened && config->errors)
    fclose(config->errors);
  config->errors = NULL;

  if(config->trace_fopened && config->trace_stream)
    fclose(config->trace_stream);
  config->trace_stream = NULL;

  Curl_safefree(config->libcurl);
}

/*
 * This is the main global destructor for the app. Call this after
 * _all_ libcurl usage is done.
 */
static void main_free(struct GlobalConfig *config)
{
  /* Cleanup the easy handle */
  curl_easy_cleanup(config->easy);
  config->easy = NULL;

  /* Main cleanup */
  curl_global_cleanup();
  convert_cleanup();
  metalink_cleanup();
#ifdef USE_NSS
  if(PR_Initialized()) {
    /* prevent valgrind from reporting still reachable mem from NSRP arenas */
    PL_ArenaFinish();
    /* prevent valgrind from reporting possibly lost memory (fd cache, ...) */
    PR_Cleanup();
  }
#endif
  free_config_fields(config);

  /* Free the config structures */
  config_free(config->last);
  config->first = NULL;
  config->last = NULL;
}

/*
 * iOS_system specifics: if the call is actually scp or sftp, we convert it to curl proper.
 * scp user@host:~/distantFile localFile => curl scp://user@host/~/distantFile -o localFile
 * scp user@host:/path/distantFile localFile => curl scp://user@host//path/distantFile -o localFile
 * scp user@host:~/distantFile . => curl scp://user@host/~/distantFile -O
 * scp user@host:~/distantFile /path/ => curl scp://user@host/~/distantFile -o /path/distantFile
 * scp localFile user@host:~/path/       => curl -T localFile scp://user@host/~/path/localFile
 */
#ifdef BLINKSHELL
__attribute__ ((visibility("default")))
int curl_static_main(int argc, char *argv[]);
#else
int curl_main(int argc, char *argv[]);
#endif

static int scp_convert(int argc, char* argv[]) {
    int argc2 = 0;
    int i = 1;
    char** argv2 = (char**) malloc((argc + 2) * sizeof(char*));
    char* localFileName = NULL;
    char* distantFileName = NULL;
    char* protocol = argv[0];
    argv2[0] = strdup("curl");
    for (i = 1, argc2 = 1; i < argc; i++, argc2++) {
        // it's just a flag:
        if ((argv[i][0] == '-') || (distantFileName && localFileName)) {
            // scp -q (quiet) is equivalent to curl -s (silent)
            if (strcmp(argv[i], "-q") == 0) argv2[argc2] = strdup("-s");
            else argv2[argc2] = strdup(argv[i]);
            continue;
        }
        char* position;
        if ((position = strstr(argv[i], ":")) != NULL) {
            // distant file
            distantFileName = position + 1; // after the ":"
            *position = 0; // split argv[i] into "user@host" and distantFileName
            asprintf(argv2 + argc2, "%s://%s/%s", protocol, argv[i], distantFileName);
            // get the actual filename
            while ((position = strstr(distantFileName, "/")) != NULL) distantFileName = position + 1;
        } else {
            // Not beginning with "-", not containing ":", must be a local filename
            // if it's ".", replace with -O
            // if it's a directory, add name of file from previous argument at the end.
            localFileName = argv[i];
            if (!distantFileName) {
                // local file before remote file: upload
                argv2[argc2] = strdup("-T"); argc2++;
                argv2[argc2] = strdup(argv[i]);
            } else { // download
                if ((strlen(argv[i]) == 1) && (strcmp(argv[i], ".") == 0)) argv2[argc2] = strdup("-O");
                else {
                    argv2[argc2] = strdup("-o"); argc2++;
                    if (argv[i][strlen(argv[i]) - 1] == '/') {
                        // if localFileName ends with '/' we assume it's a directory
                        asprintf(argv2 + argc2, "%s%s", localFileName, distantFileName);
                    } else {
                        struct stat localFileBuf;
                        bool localFileExists = (stat(localFileName, &localFileBuf) == 0);
                        int localFileIsDir = S_ISDIR(localFileBuf.st_mode);
                        if (localFileExists && localFileIsDir) {
                            // localFileName exists *and* is a directory: concatenate distantFileName to directory
                            asprintf(argv2 + argc2, "%s/%s", localFileName, distantFileName);
                        } else {
                            // all other cases: localFileName is name of output
                            argv2[argc2] = strdup(argv[i]);
                        }
                    }
                }
            }
        }
    }
    argv2[argc2] = NULL;
    int returnValue = -1;
    if (!(localFileName && distantFileName)) {
        fprintf(thread_stderr, "Usage:\t%s [-q] [user@]host:distantFile localFile\n", protocol);
        fprintf(thread_stderr, "\t%s [-q] localFile [user@]host:distantFile \n", protocol);
    } else {
#ifdef BLINKSHELL
        returnValue = curl_static_main(argc2, argv2);
#else
        returnValue = curl_main(argc2, argv2);
#endif
    }
    for (int i = 0; i < argc2; i++) free(argv2[i]);
    free(argv2);
    return returnValue;
}
/*
** curl tool main function.
*/
#ifdef BLINKSHELL
int curl_static_main(int argc, char *argv[])
#else
int curl_main(int argc, char *argv[])
#endif
{
    // scp, sftp: edit arguments and relaunch
    if ((strcmp(argv[0], "scp") == 0) || (strcmp(argv[0], "sftp") == 0)) {
        return scp_convert(argc, argv);
    }
  CURLcode result = CURLE_OK;
  struct GlobalConfig global;
  memset(&global, 0, sizeof(global));

  main_checkfds();

#if defined(HAVE_SIGNAL) && defined(SIGPIPE)
  (void)signal(SIGPIPE, SIG_IGN);
#endif

  /* Initialize memory tracking */
  memory_tracking_init();

  /* Initialize the curl library - do not call any libcurl functions before
     this point */
  result = main_init(&global);
  if(!result) {
    /* Start our curl operation */
    result = operate(&global, argc, argv);

#ifdef __SYMBIAN32__
    if(global.showerror)
      tool_pressanykey();
#endif

    /* Perform the main cleanup */
    main_free(&global);
  }

#ifdef __NOVELL_LIBC__
  if(getenv("_IN_NETWARE_BASH_") == NULL)
    tool_pressanykey();
#endif

#ifdef __VMS
  vms_special_exit(result, vms_show);
#else
  return (int)result;
#endif
}

#endif /* ndef UNITTESTS */

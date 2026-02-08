#ifndef CRUN_BINDING_H
#define CRUN_BINDING_H

#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

/* libcrun headers */
#include <libcrun/container.h>
#include <libcrun/status.h>
#include <libcrun/linux.h>
#include <libcrun/utils.h>

#define NAPI_CALL(env, call)                                      \
  do {                                                            \
    napi_status status = (call);                                  \
    if (status != napi_ok) {                                      \
      napi_throw_error((env), NULL, "N-API call failed");         \
      return NULL;                                                \
    }                                                             \
  } while(0)

/* Function declarations */
char* napi_get_string(napi_env env, napi_value value);
napi_value napi_create_error_obj(napi_env env, int code, const char* msg);

#endif /* CRUN_BINDING_H */
/* napi.c — the Node-API binding for the r3 engine (engine.c).
 *
 * Written fresh in this session. One export, buildAll(buffer): the raw
 * cases.json bytes in, the answers.json bytes out as a zero-copy external
 * Buffer — the engine's malloc'd output, released by the finalizer when V8
 * collects the Buffer. N-API is ABI-stable, so this module loads on Node 22
 * and Node 24 alike. When the module cannot load, main.ts falls back to the
 * pure-JavaScript solve.js.
 */
#include <node_api.h>
#include <stdlib.h>

unsigned char *build_all_alloc(const unsigned char *data, size_t len, size_t *out_len);

static void release_output(napi_env env, void *data, void *hint) {
  (void)env;
  (void)hint;
  free(data);
}

static napi_value call_build_all(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value args[1];
  if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok) return NULL;
  if (argc < 1) {
    napi_throw_type_error(env, NULL, "buildAll takes one Buffer argument");
    return NULL;
  }
  void *bytes = NULL;
  size_t nbytes = 0;
  if (napi_get_buffer_info(env, args[0], &bytes, &nbytes) != napi_ok) {
    napi_throw_type_error(env, NULL, "buildAll takes one Buffer argument");
    return NULL;
  }
  size_t out_len = 0;
  unsigned char *out = build_all_alloc((const unsigned char *)bytes, nbytes, &out_len);
  if (out == NULL) {
    napi_throw_error(env, NULL, "build_all_alloc failed");
    return NULL;
  }
  napi_value result;
  if (napi_create_external_buffer(env, out_len, out, release_output, NULL, &result) != napi_ok) {
    free(out);
    napi_throw_error(env, NULL, "could not wrap the output buffer");
    return NULL;
  }
  return result;
}

NAPI_MODULE_INIT() {
  napi_value fn;
  if (napi_create_function(env, "buildAll", NAPI_AUTO_LENGTH, call_build_all, NULL, &fn) != napi_ok) return NULL;
  if (napi_set_named_property(env, exports, "buildAll", fn) != napi_ok) return NULL;
  return exports;
}

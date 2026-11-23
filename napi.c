/* napi.c — the Node-API wrapper for engine.c's build_all.
 *
 * Written fresh in this session. Exposes one function, buildAll(buffer):
 * it takes the raw cases.json bytes and returns the answers.json bytes as a
 * zero-copy external Buffer (the C side's malloc'd output, freed by the
 * finalizer). N-API is ABI-stable, so a module built against this header
 * loads on Node 22 and Node 24 alike. If the module fails to load for any
 * reason, solve.js falls back to its pure-JavaScript run().
 */
#include <node_api.h>
#include <stdlib.h>

char *build_all(const char *json, size_t len, size_t *out_len);

static void free_out(napi_env env, void *data, void *hint) {
  (void)env;
  (void)hint;
  free(data);
}

static napi_value BuildAll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
  if (argc < 1) {
    napi_throw_type_error(env, NULL, "buildAll expects a Buffer");
    return NULL;
  }
  void *data = NULL;
  size_t len = 0;
  if (napi_get_buffer_info(env, argv[0], &data, &len) != napi_ok) {
    napi_throw_type_error(env, NULL, "buildAll expects a Buffer");
    return NULL;
  }
  size_t out_len = 0;
  char *out = build_all((const char *)data, len, &out_len);
  if (out == NULL) {
    napi_throw_error(env, NULL, "build_all failed");
    return NULL;
  }
  napi_value buf;
  if (napi_create_external_buffer(env, out_len, out, free_out, NULL, &buf) != napi_ok) {
    free(out);
    napi_throw_error(env, NULL, "could not create the output buffer");
    return NULL;
  }
  return buf;
}

NAPI_MODULE_INIT() {
  napi_value fn;
  if (napi_create_function(env, NULL, 0, BuildAll, NULL, &fn) != napi_ok) return NULL;
  if (napi_set_named_property(env, exports, "buildAll", fn) != napi_ok) return NULL;
  return exports;
}

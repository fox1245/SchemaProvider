/* Example C ABI over SchemaProvider for foreign-function hosts (Python ctypes, C#, Rust, ...).
 *
 * This is a worked example, NOT a supported product API: the SDK ships no C ABI and no Python
 * package. Everything crosses the boundary as UTF-8 text. The typed conversation (including the
 * SDK's authenticated native replay state) stays inside this library; the host only sees JSON.
 *
 * Ownership: every `char*` returned by this library is allocated by it and must be released with
 * sp_capi_string_free exactly once. Handles are released with their *_destroy function. No
 * function throws; failures are reported in-band as {"ok":false,"error":{"kind","message"}} or,
 * for creation functions, through *error. Allocation failure may return NULL without an error.
 *
 * Threading: a client may be shared by many threads. A conversation serializes its own sends;
 * use one conversation per independent chat. Destroy must not race with any use of that raw
 * handle. A conversation retains the SDK client, so closing its parent handle is safe.
 */
#ifndef SP_CAPI_H
#define SP_CAPI_H

#include <stdint.h>

#if defined(_WIN32)
#  ifdef SP_CAPI_BUILD
#    define SP_CAPI_API __declspec(dllexport)
#  else
#    define SP_CAPI_API __declspec(dllimport)
#  endif
#else
#  define SP_CAPI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sp_capi_client sp_capi_client;
typedef struct sp_capi_conversation sp_capi_conversation;

/* Interface revision of the SchemaProvider this library was built against. */
SP_CAPI_API uint32_t sp_capi_interface_revision(void);

/* Frees a string returned by this library (NULL is allowed). */
SP_CAPI_API void sp_capi_string_free(char* text);

/* descriptor_json: a SchemaProvider descriptor (family must be "openai.chat" in this example).
 * api_key: credential text, or NULL.
 * dotenv_path + api_key_name: alternatively read the credential from a .env file with
 *   cppdotenv without returning it to Python or exporting it. Native and Python share one process;
 *   this is not secret isolation or secure memory erasure.
 * On failure returns NULL and, when error is non-NULL, stores a message to free. */
SP_CAPI_API sp_capi_client* sp_capi_client_create(const char* descriptor_json, const char* api_key,
                                                  const char* dotenv_path, const char* api_key_name,
                                                  char** error);
SP_CAPI_API void sp_capi_client_destroy(sp_capi_client* client);

/* tools_json: JSON array of {"name","description","parameters"} or NULL. */
SP_CAPI_API sp_capi_conversation* sp_capi_conversation_create(sp_capi_client* client, const char* model,
                                                              const char* system, uint32_t max_output_tokens,
                                                              const char* tools_json, char** error);
SP_CAPI_API void sp_capi_conversation_destroy(sp_capi_conversation* conversation);

/* turn_json is {"user":"text"} or {"tool_results":[{"call_id":"..","content":".."}]}.
 * Blocks for one SDK attempt. timeout_ms starts after handle/queue locking and history building
 * (0 selects 30 seconds); it is not a whole-call queue-wait bound. CDLL may release the host GIL.
 * Returns {"ok":true,"text":..,"tool_calls":[{"id","name","arguments"}],"stop":..,"usage":{..}}
 * or {"ok":false,"error":{"kind":..,"message":..}}. A failed send leaves the history unchanged. */
SP_CAPI_API char* sp_capi_conversation_send(sp_capi_conversation* conversation, const char* turn_json,
                                            int streaming, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif

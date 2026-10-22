/* Null client for the SDK comparison: the floor for what an HTTP/1.1 client can do on the same peer.
 *
 * One thread, libcurl's multi interface, one reusable easy handle per in-flight slot. The request body is
 * serialized once, nothing is parsed, and every reply is only counted: its byte length and its number of SSE
 * event terminators (or, for a buffered reply, whether it contains the expected text). Every reply must have the
 * length and the event count of the first reply, and the first reply must carry the expected text; the checks
 * run after the request's clock has stopped.
 *
 * `--fresh` forbids connection reuse. It is the floor for an SDK that opens a connection per request.
 *
 * The line protocol on stdout and stdin is the one in ../../README.md. */
#define _GNU_SOURCE
#include <curl/curl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  const char *api, *mode, *base, *ca, *system_text, *user_text, *model, *expect, *key;
  int concurrency, warmup, requests, max_tokens, chunks, cold, fresh;
} Cfg;

typedef struct {
  CURL *easy;
  char *buf;
  size_t len, cap;
  int64_t t0;
  int index; /* request number, -1 when idle */
} Slot;

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static size_t on_body(char *data, size_t size, size_t count, void *user) {
  Slot *slot = user;
  size_t n = size * count;
  if (slot->len + n + 1 > slot->cap) {
    size_t cap = slot->cap ? slot->cap : 4096;
    while (cap < slot->len + n + 1) cap *= 2;
    slot->buf = realloc(slot->buf, cap);
    slot->cap = cap;
  }
  memcpy(slot->buf + slot->len, data, n);
  slot->len += n;
  slot->buf[slot->len] = 0;
  return n;
}

static long kb_of(const char *field) {
  FILE *f = fopen("/proc/self/status", "r");
  char line[256];
  long value = -1;
  size_t n = strlen(field);
  while (f && fgets(line, sizeof line, f))
    if (!strncmp(line, field, n)) { value = strtol(line + n, NULL, 10); break; }
  if (f) fclose(f);
  return value;
}

static void usage_us(long *user, long *sys) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  *user = ru.ru_utime.tv_sec * 1000000L + ru.ru_utime.tv_usec;
  *sys = ru.ru_stime.tv_sec * 1000000L + ru.ru_stime.tv_usec;
}

#include <stdarg.h>
static char *format(const char *fmt, ...) {
  va_list args;
  char *out = NULL;
  va_start(args, fmt);
  if (vasprintf(&out, fmt, args) < 0) { perror("vasprintf"); exit(2); }
  va_end(args);
  return out;
}

static char *json_quote(const char *s) {
  size_t n = strlen(s);
  char *out = malloc(n * 2 + 3), *p = out;
  *p++ = '"';
  for (; *s; ++s) {
    if (*s == '"' || *s == '\\') *p++ = '\\';
    *p++ = *s;
  }
  *p++ = '"';
  *p = 0;
  return out;
}

static char *build_body(const Cfg *c) {
  char *sys = json_quote(c->system_text), *user = json_quote(c->user_text), *model = json_quote(c->model);
  const char *stream = !strcmp(c->mode, "stream") ? "true" : "false";
  char *out = NULL;
  if (!strcmp(c->api, "openai"))
    out = format("{\"model\":%s,\"messages\":[{\"role\":\"system\",\"content\":%s},{\"role\":\"user\",\"content\":%s}],\"max_tokens\":%d,\"stream\":%s}",
             model, sys, user, c->max_tokens, stream);
  else
    out = format("{\"model\":%s,\"max_tokens\":%d,\"system\":%s,\"messages\":[{\"role\":\"user\",\"content\":%s}],\"stream\":%s}",
             model, c->max_tokens, sys, user, stream);
  free(sys); free(user); free(model);
  return out;
}

static int count_events(const char *s, size_t n) {
  int events = 0;
  for (size_t i = 0; i + 1 < n; ++i)
    if (s[i] == '\n' && s[i + 1] == '\n') ++events;
  return events;
}

typedef struct {
  const Cfg *cfg;
  CURLM *multi;
  Slot *slots;
  char *url, *body;
  struct curl_slist *headers;
  size_t ref_len;  /* length of the first reply, 0 until known */
  int ref_events;
  int ok, failed, verify_failed;
  char first_error[256];
  int64_t *latency; /* per measured request */
} Run;

static void fail(Run *r, const char *what) {
  if (!r->failed++) snprintf(r->first_error, sizeof r->first_error, "%s", what);
}

/* Start request `index` on an idle slot. */
static void begin(Run *r, Slot *slot, int index) {
  slot->len = 0;
  if (slot->buf) slot->buf[0] = 0;
  slot->index = index;
  slot->t0 = now_ns();
  curl_multi_add_handle(r->multi, slot->easy);
}

static void check_reply(Run *r, const Slot *slot, int measured) {
  size_t len = slot->len;
  int events = count_events(slot->buf, len);
  if (r->cfg->chunks == 0) { /* replay: recorded replies differ in length and event count; check each one on its own */
    int ok = !strcmp(r->cfg->mode, "stream") ? events > 0 : strstr(slot->buf, r->cfg->expect) != NULL;
    if (!ok) { if (measured) r->verify_failed++; fail(r, "reply does not carry the expected text"); }
    return;
  }
  if (!r->ref_len) { /* the first reply defines the shape, and must carry the expected text */
    int ok = !strcmp(r->cfg->mode, "stream") ? events >= r->cfg->chunks : strstr(slot->buf, r->cfg->expect) != NULL;
    if (!ok) { if (measured) r->verify_failed++; fail(r, "first reply does not carry the expected text"); return; }
    r->ref_len = len; r->ref_events = events;
  } else if (len != r->ref_len || events != r->ref_events) {
    if (measured) r->verify_failed++;
  }
}

/* Run `count` requests with at most `concurrency` in flight. Returns the wall time in ns. */
static int64_t run_phase(Run *r, int count, int record) {
  const Cfg *c = r->cfg;
  int started = 0, finished = 0, running = 0;
  int64_t t_begin = now_ns();
  int slots = c->concurrency < count ? c->concurrency : count;
  for (int i = 0; i < slots; ++i) begin(r, &r->slots[i], started++);
  while (finished < count) {
    curl_multi_perform(r->multi, &running);
    int queued;
    CURLMsg *msg;
    while ((msg = curl_multi_info_read(r->multi, &queued))) {
      if (msg->msg != CURLMSG_DONE) continue;
      int64_t t1 = now_ns();
      Slot *slot = NULL;
      for (int i = 0; i < c->concurrency; ++i) if (r->slots[i].easy == msg->easy_handle) { slot = &r->slots[i]; break; }
      long code = 0;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &code);
      curl_multi_remove_handle(r->multi, msg->easy_handle);
      if (msg->data.result != CURLE_OK) {
        char what[160];
        snprintf(what, sizeof what, "transfer failed: %s", curl_easy_strerror(msg->data.result));
        fail(r, what);
      } else if (code != 200) {
        char what[64];
        snprintf(what, sizeof what, "HTTP status %ld", code);
        fail(r, what);
      } else {
        if (record) { r->latency[slot->index] = t1 - slot->t0; r->ok++; }
        check_reply(r, slot, record); /* after the clock stopped */
      }
      ++finished;
      if (started < count) begin(r, slot, started++); else slot->index = -1;
    }
    if (finished < count) curl_multi_poll(r->multi, NULL, 0, 1000, NULL);
  }
  return now_ns() - t_begin;
}

static void print_result(const Run *r, int64_t wall, long cpu_user, long cpu_sys, long rss_ready, long hwm_ready) {
  const Cfg *c = r->cfg;
  printf("{\"event\":\"result\",\"schema\":1,\"client\":\"%s\",\"version\":\"%s\",\"api\":\"%s\",\"mode\":\"%s\",\"concurrency\":%d,"
         "\"warmup\":%d,\"requests\":%d,\"ok\":%d,\"failed\":%d,\"first_error\":%s%s%s,\"verify_failed\":%d,"
         "\"wall_ns\":%lld,\"cpu_user_us\":%ld,\"cpu_sys_us\":%ld,\"rss_ready_kb\":%ld,\"hwm_ready_kb\":%ld,"
         "\"rss_end_kb\":%ld,\"hwm_end_kb\":%ld,"
         "\"config\":{\"concurrency_model\":\"libcurl multi interface, one thread, %d reusable easy handles\","
         "\"http\":\"1.1\",\"pool\":\"CURLMOPT_MAXCONNECTS and CURLMOPT_MAX_HOST_CONNECTIONS = %d, connection reuse %s\","
         "\"ca\":\"CURLOPT_CAINFO\",\"retries\":\"none\",\"telemetry\":\"none\",\"libcurl\":\"%s\"},\"latency_ns\":[",
         c->fresh ? "null-curl-fresh" : "null-curl", curl_version_info(CURLVERSION_NOW)->version, c->api, c->mode, c->concurrency, c->warmup, c->requests,
         r->ok, r->failed, r->failed ? "\"" : "", r->failed ? r->first_error : "null", r->failed ? "\"" : "", r->verify_failed,
         (long long)wall, cpu_user, cpu_sys, rss_ready, hwm_ready, kb_of("VmRSS:"), kb_of("VmHWM:"),
         c->concurrency, c->concurrency, c->fresh ? "forbidden" : "on", curl_version());
  for (int i = 0; i < c->requests; ++i) printf("%s%lld", i ? "," : "", (long long)r->latency[i]);
  printf("]}\n");
  fflush(stdout);
}

static const char *arg_value(int argc, char **argv, const char *name, const char *fallback) {
  for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], name)) return argv[i + 1];
  return fallback;
}
static int arg_flag(int argc, char **argv, const char *name) {
  for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], name)) return 1;
  return 0;
}

int main(int argc, char **argv) {
  Cfg c = {
    .api = arg_value(argc, argv, "--api", "openai"), .mode = arg_value(argc, argv, "--mode", "buffered"),
    .base = arg_value(argc, argv, "--base-prefix", ""), .ca = arg_value(argc, argv, "--ca", getenv("SSL_CERT_FILE")),
    .system_text = arg_value(argc, argv, "--system", ""), .user_text = arg_value(argc, argv, "--user", ""),
    .model = arg_value(argc, argv, "--model", "bench-model"), .expect = arg_value(argc, argv, "--expect", ""),
    .key = arg_value(argc, argv, "--api-key", ""),
    .concurrency = atoi(arg_value(argc, argv, "--concurrency", "1")), .warmup = atoi(arg_value(argc, argv, "--warmup", "0")),
    .requests = atoi(arg_value(argc, argv, "--requests", "1")), .max_tokens = atoi(arg_value(argc, argv, "--max-tokens", "256")),
    .chunks = atoi(arg_value(argc, argv, "--chunks", "1")), .cold = arg_flag(argc, argv, "--cold"), .fresh = arg_flag(argc, argv, "--fresh"),
  };
  if (!c.ca || !*c.base || c.concurrency < 1) { fprintf(stderr, "usage: see README\n"); return 2; }
  if (c.cold) { c.concurrency = 1; c.warmup = 0; c.requests = 1; }
  const int stream = !strcmp(c.mode, "stream");

  curl_global_init(CURL_GLOBAL_DEFAULT);
  Run r = { .cfg = &c };
  r.multi = curl_multi_init();
  if (!arg_flag(argc, argv, "--default-cache")) /* experiment: leave libcurl's connection cache at its default size */
    curl_multi_setopt(r.multi, CURLMOPT_MAXCONNECTS, (long)c.concurrency);
  curl_multi_setopt(r.multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)c.concurrency);
  r.url = format("%s/v1/%s", c.base, !strcmp(c.api, "openai") ? "chat/completions" : "messages");
  r.body = build_body(&c);
  char line[512];
  r.headers = curl_slist_append(r.headers, "Content-Type: application/json");
  r.headers = curl_slist_append(r.headers, stream ? "Accept: text/event-stream" : "Accept: application/json");
  if (!strcmp(c.api, "openai")) snprintf(line, sizeof line, "Authorization: Bearer %s", c.key);
  else { r.headers = curl_slist_append(r.headers, "anthropic-version: 2023-06-01"); snprintf(line, sizeof line, "x-api-key: %s", c.key); }
  r.headers = curl_slist_append(r.headers, line);
  r.slots = calloc((size_t)c.concurrency, sizeof(Slot));
  r.latency = calloc((size_t)c.requests + 1, sizeof(int64_t));
  for (int i = 0; i < c.concurrency; ++i) {
    CURL *e = r.slots[i].easy = curl_easy_init();
    r.slots[i].index = -1;
    curl_easy_setopt(e, CURLOPT_URL, r.url);
    curl_easy_setopt(e, CURLOPT_POST, 1L);
    curl_easy_setopt(e, CURLOPT_POSTFIELDS, r.body);
    curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)strlen(r.body));
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, r.headers);
    curl_easy_setopt(e, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(e, CURLOPT_CAINFO, c.ca);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(e, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &r.slots[i]);
    if (c.fresh) { curl_easy_setopt(e, CURLOPT_FRESH_CONNECT, 1L); curl_easy_setopt(e, CURLOPT_FORBID_REUSE, 1L); }
  }

  if (c.cold) {
    run_phase(&r, 1, 1);
    printf("{\"event\":\"first\",\"ok\":%s}\n", r.ok == 1 && !r.verify_failed ? "true" : "false");
    fflush(stdout);
    return r.ok == 1 ? 0 : 1;
  }
  long rss_ready, hwm_ready;
  for (;;) { /* warm-up rounds: the runner answers each `ready` with MORE or GO */
    if (c.warmup) run_phase(&r, c.warmup, 0);
    rss_ready = kb_of("VmRSS:");
    hwm_ready = kb_of("VmHWM:");
    printf("{\"event\":\"ready\"}\n");
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) return 3;
    if (!strncmp(line, "GO", 2)) break;
    if (strncmp(line, "MORE", 4)) return 3;
  }

  long u0, s0, u1, s1;
  usage_us(&u0, &s0);
  int64_t wall = run_phase(&r, c.requests, 1);
  usage_us(&u1, &s1);
  print_result(&r, wall, u1 - u0, s1 - s0, rss_ready, hwm_ready);
  return 0;
}

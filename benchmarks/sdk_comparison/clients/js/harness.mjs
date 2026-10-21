// Shared measurement loop for the Node contenders; the JavaScript twin of ../py/harness.py.
//
//   setup(args)            build the SDK client once
//   call(ctx, args)        one request. Buffered: returns the SDK's result. Streamed: consumes every chunk and returns the
//                          array of non-empty text deltas it saw.
//   extract(raw)           buffered only: the response text, read after the request's clock has stopped
//   describe(args)         the settings the client actually used
import { readFileSync } from 'node:fs';

export function parseArgs(argv) {
  const a = { cold: false };
  const numeric = new Set(['concurrency', 'warmup', 'requests', 'max-tokens', 'chunks']);
  for (let i = 2; i < argv.length; i++) {
    const token = argv[i];
    if (!token.startsWith('--')) throw new Error(`unexpected argument ${token}`);
    const key = token.slice(2);
    if (key === 'cold') { a.cold = true; continue; }
    const name = key.replace(/-([a-z])/g, (_, c) => c.toUpperCase());
    a[name] = numeric.has(key) ? Number(argv[++i]) : argv[++i];
  }
  return a;
}

function statusKb(field) {
  for (const line of readFileSync('/proc/self/status', 'utf8').split('\n')) if (line.startsWith(field)) return Number(line.split(/\s+/)[1]);
  return -1;
}

function verify(a, payload) {
  if (a.mode === 'stream') return Array.isArray(payload) && payload.length === a.chunks && payload.join('') === a.expect;
  return payload === a.expect;
}

async function drive(ctx, call, extract, a, count, record) {
  const latency = record ? new Array(count).fill(0) : null;
  const payload = record ? new Array(count) : null;
  let next = 0, failed = 0, firstError = null;
  const worker = async () => {
    for (;;) {
      const i = next++;
      if (i >= count) return;
      const t0 = process.hrtime.bigint();
      let raw;
      try { raw = await call(ctx, a); } catch (error) { failed++; firstError ??= `${error?.name ?? 'Error'}: ${error?.message ?? error}`.slice(0, 300); continue; }
      const t1 = process.hrtime.bigint();
      if (record) { latency[i] = Number(t1 - t0); payload[i] = a.mode === 'stream' ? raw : extract(raw); }
    }
  };
  await Promise.all(Array.from({ length: Math.min(a.concurrency, count) }, worker));
  return { latency, payload, failed, firstError };
}

const emit = (obj, then) => process.stdout.write(JSON.stringify(obj) + '\n', then);

export async function main({ client, version, setup, call, extract, describe }) {
  const a = parseArgs(process.argv);
  if (a.cold) a.concurrency = 1;
  const ctx = await setup(a);
  if (a.cold) {
    const run = await drive(ctx, call, extract, a, 1, true);
    const ok = run.failed === 0 && verify(a, run.payload[0]);
    emit({ event: 'first', ok }, () => process.exit(ok ? 0 : 1));
    return;
  }
  let warmFailed = 0, warmError = null, rssReady, hwmReady;
  const readLine = () => new Promise((resolve) => {
    process.stdin.setEncoding('utf8');
    process.stdin.once('data', (chunk) => { process.stdin.pause(); resolve(chunk); });
    process.stdin.resume();
  });
  for (;;) {
    const warm = a.warmup ? await drive(ctx, call, extract, a, a.warmup, false) : { failed: 0, firstError: null };
    warmFailed += warm.failed; warmError ??= warm.firstError;
    rssReady = statusKb('VmRSS:'); hwmReady = statusKb('VmHWM:');
    emit({ event: 'ready' });
    const reply = await readLine();
    if (reply.startsWith('GO')) break;
    if (!reply.startsWith('MORE')) process.exit(3);
  }
  const r0 = process.resourceUsage();
  const t0 = process.hrtime.bigint();
  const run = await drive(ctx, call, extract, a, a.requests, true);
  const t1 = process.hrtime.bigint();
  const r1 = process.resourceUsage();
  // Outside the measured window.
  const bad = run.payload.filter((p) => p !== undefined && !verify(a, p)).length;
  emit({
    event: 'result', schema: 1, client, version, api: a.api, mode: a.mode, concurrency: a.concurrency, warmup: a.warmup,
    requests: a.requests, ok: a.requests - run.failed, failed: run.failed + warmFailed, first_error: run.firstError ?? warmError,
    verify_failed: bad, wall_ns: Number(t1 - t0), cpu_user_us: r1.userCPUTime - r0.userCPUTime, cpu_sys_us: r1.systemCPUTime - r0.systemCPUTime,
    rss_ready_kb: rssReady, hwm_ready_kb: hwmReady, rss_end_kb: statusKb('VmRSS:'), hwm_end_kb: statusKb('VmHWM:'),
    config: describe(a), latency_ns: run.latency,
  }, () => process.exit(0));
}

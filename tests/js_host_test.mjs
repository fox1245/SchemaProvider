// The JavaScript reference host (src/runtime/js_host.h) under Node, against a local HTTP server started here: a
// buffered and an SSE chat through the fetch transport, the failures a Client reports, cancellation and deadlines,
// the transport's own contract (redirects, backpressure, completions that never run inline) and the executor.
// No network and no keys. Usage: node tests/js_host_test.mjs <path to sp_js_host_tests.js> [path to sp_js_host_example.js]
import assert from 'node:assert/strict';
import http from 'node:http';
import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);
const createModule = require(path.resolve(process.argv[2]));

const text = '안녕 \u{1F600} ok';  // "안녕 😀 ok"
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// The chat completion chunks the server streams, as bytes, so that it can cut them anywhere.
function sseBody() {
  const event = (delta, finish) => 'data: ' + JSON.stringify({
    id: 's', object: 'chat.completion.chunk', created: 1, model: 'stream-model',
    choices: [{ index: 0, delta, finish_reason: finish }],
    ...(finish ? { usage: { prompt_tokens: 2, completion_tokens: 4, total_tokens: 6 } } : {}),
  }) + '\n\n';
  return Buffer.from(event({ role: 'assistant', content: '안녕 ' }, null) + event({ content: '\u{1F600} ok' }, null) +
                     event({}, 'stop') + 'data: [DONE]\n\n', 'utf8');
}
const sse = sseBody();
// Where the server cuts the stream: inside the first line, inside a three-byte character, between the two newlines
// that end an event, inside a four-byte character, and once more near the end.
const cuts = (() => {
  const first = sse.indexOf(Buffer.from('안', 'utf8'));
  const emoji = sse.indexOf(Buffer.from('\u{1F600}', 'utf8'));
  const blank = sse.indexOf('\n\n') + 1;
  return [20, first + 1, blank, emoji + 2, sse.length - 7];
})();

const stats = { requests: {}, closed: 0, echo: null };
const server = http.createServer(async (req, res) => {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  const body = Buffer.concat(chunks).toString('utf8');
  const url = new URL(req.url, 'http://x');
  let route = url.pathname.slice(1);
  if (route.endsWith('chat/completions')) route = JSON.parse(body).model;
  stats.requests[route] = (stats.requests[route] ?? 0) + 1;
  res.on('close', () => { if (!res.writableFinished) stats.closed += 1; });
  const sendEvent = (delta, finish) => res.write('data: ' + JSON.stringify({
    id: 's', object: 'chat.completion.chunk', created: 1, model: 'x', choices: [{ index: 0, delta, finish_reason: finish }] }) + '\n\n');
  switch (route) {
    case 'buffered':
      res.writeHead(200, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ id: 'c1', model: 'buffered-model', created: 7, object: 'chat.completion',
        choices: [{ index: 0, message: { role: 'assistant', content: 'hello from fetch' }, finish_reason: 'stop' }],
        usage: { prompt_tokens: 2, completion_tokens: 4, total_tokens: 6 } }));
      break;
    case 'stream': {
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      let at = 0;
      for (const cut of [...cuts, sse.length]) {
        res.write(sse.subarray(at, cut));
        at = cut;
        await sleep(40);
      }
      res.end();
      break;
    }
    case 'limit':
      res.writeHead(429, { 'content-type': 'application/json' });
      res.end(JSON.stringify({ error: { message: 'slow down', type: 'rate_limit_error', code: 'rate_limit_exceeded' } }));
      break;
    case 'slow':
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      sendEvent({ role: 'assistant', content: 'first ' }, null);
      for (let i = 0; i < 100 && !res.destroyed; ++i) { await sleep(50); if (!res.destroyed) sendEvent({ content: 'more ' }, null); }
      res.end();
      break;
    case 'stall':
      res.writeHead(200, { 'content-type': 'text/event-stream' });
      sendEvent({ role: 'assistant', content: 'partial ' }, null);
      break;  // and nothing more, for as long as the client waits
    case 'hang':
    case 'abort':
      break;
    case 'first-byte':
      await sleep(1500);
      res.writeHead(200, { 'content-type': 'text/plain' });
      res.end('late');
      break;
    case 'echo':
      stats.echo = { method: req.method, headers: req.headers, body };
      res.writeHead(200, { 'content-type': 'application/json', 'x-answer': 'yes' });
      res.end(JSON.stringify(stats.echo));
      break;
    case 'redirect':
      res.writeHead(302, { location: '/echo' });
      res.end();
      break;
    case 'big':
      // 24 MiB: more than the module has to begin with, so its memory must grow while the chunks cross.
      res.writeHead(200, { 'content-type': 'application/octet-stream' });
      for (let i = 0; i < 12 && !res.destroyed; ++i) { res.write(Buffer.alloc(2 * 1024 * 1024, 0x30 + i)); await sleep(5); }
      res.end();
      break;
    case 'backpressure':
      res.writeHead(200, { 'content-type': 'application/octet-stream' });
      for (let i = 0; i < 8 && !res.destroyed; ++i) { res.write(Buffer.alloc(16384, 0x61 + i)); await sleep(15); }
      res.end();
      break;
    default:
      res.writeHead(404);
      res.end();
  }
});
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;
// A port nothing listens on.
const dead = await new Promise((resolve) => {
  const probe = http.createServer().listen(0, '127.0.0.1', () => { const port = probe.address().port; probe.close(() => resolve(`http://127.0.0.1:${port}`)); });
});

const waiting = new Map();
const early = new Map();
const mod = await createModule({
  onReport(name, json) {
    if (name === 'fatal') { console.error(`fatal: ${json}`); process.exit(1); }
    const report = JSON.parse(json);
    if (waiting.has(name)) { const resolve = waiting.get(name); waiting.delete(name); resolve(report); }
    else early.set(name, report);
  },
});
// The next report of that name; `start` begins the scenario that makes it.
function report(name, start) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${name}: no report`)), 15000);
    const done = (value) => { clearTimeout(timer); resolve(value); };
    if (early.has(name)) { done(early.get(name)); early.delete(name); return; }
    waiting.set(name, done);
    if (start) start();
  });
}
const until = (name) => report(name);
function run(scenario, url = base, reportName = scenario) {
  return report(reportName, () => mod.ccall('sp_test_run', null, ['string', 'string'], [scenario, url]));
}
async function check(name, body) {
  try { await body(); console.log(`ok   ${name}`); }
  catch (error) { console.error(`FAIL ${name}`); throw error; }
}
// The cumulative offsets at which the fetch transport cut the stream.
const boundaries = (chunks) => chunks.reduce((all, size) => [...all, (all.at(-1) ?? 0) + size], []).slice(0, -1);

await check('build', async () => {
  const report = await run('build');
  assert.equal(report.threads, 0);
  assert.equal(report.size_t, 4);
});

await check('buffered chat', async () => {
  const report = await run('chat_buffered');
  assert.equal(report.ok, true, JSON.stringify(report));
  assert.equal(report.text, 'hello from fetch');
  assert.equal(stats.requests.buffered, 1);
});

await check('streamed chat, cut inside events and inside UTF-8 sequences', async () => {
  const report = await run('chat_stream');
  assert.equal(report.ok, true, JSON.stringify(report));
  assert.equal(report.text, text);
  assert.equal(report.streamed, text);
  assert.ok(report.events > 3);
  // The transport handed the framer the pieces as the network delivered them; at least two of the cuts must have
  // survived to the module, one inside an event and one inside a multi-byte character.
  const cutAt = boundaries(report.chunks);
  assert.ok(cutAt.length >= 2, `chunks ${report.chunks}`);
  const inside = (offset) => (sse[offset] & 0xc0) === 0x80;
  assert.ok(cutAt.some(inside), `a cut inside a UTF-8 sequence: ${cutAt}`);
  assert.ok(cutAt.some((offset) => !inside(offset) && !(sse[offset - 1] === 0x0a && sse[offset - 2] === 0x0a)), `a cut inside an event: ${cutAt}`);
  assert.equal(report.chunks.reduce((sum, size) => sum + size, 0), sse.length);
});

await check('an HTTP error is the server\'s answer', async () => {
  const report = await run('chat_http_error');
  assert.equal(report.ok, false);
  assert.equal(report.http_status, 429);
  assert.equal(report.sent, true);
  assert.equal(report.safety, 'PossiblyAccepted');
});

await check('cancel aborts the fetch', async () => {
  const closedBefore = stats.closed;
  const report = await run('chat_cancel');
  assert.equal(report.ok, false);
  assert.equal(report.kind, 'Cancelled');
  assert.match(report.streamed, /^first/);
  for (let i = 0; i < 50 && stats.closed === closedBefore; ++i) await sleep(20);
  assert.equal(stats.closed, closedBefore + 1, 'the server saw the connection close');
});

await check('an unreachable server is a transport failure', async () => {
  const report = await run('chat_unreachable', dead);
  assert.equal(report.ok, false);
  assert.equal(report.kind, 'Transport');
});

await check('the deadline, kept by the executor\'s timers', async () => {
  const report = await run('chat_deadline');
  assert.equal(report.ok, false);
  assert.equal(report.kind, 'DeadlineExceeded');
});

await check('an idle stream ends at the idle bound', async () => {
  const report = await run('chat_idle_stall');
  assert.equal(report.ok, false);
  assert.equal(report.kind, 'Truncated');
  assert.match(report.streamed, /^partial/);
});

await check('blocking calls are refused', async () => {
  const refusal = await run('chat_blocking', base, 'chat_blocking_refusal');
  assert.equal(refusal.join_refused, true);
  assert.equal(refusal.complete_refused, true);
  assert.equal(refusal.requests_before_pump, 0);
  const final = await until('chat_blocking');
  assert.equal(final.ok, true);
  assert.equal(final.requests, 1);
});

await check('a destroyed Client ends its call when the loop turns', async () => {
  const closedBefore = stats.closed;
  const slowBefore = stats.requests.slow;
  const final = report('chat_destroy', () => mod.ccall('sp_test_run', null, ['string', 'string'], ['chat_destroy', base]));
  for (let i = 0; i < 100 && stats.requests.slow === slowBefore; ++i) await sleep(20);
  assert.equal(stats.requests.slow, slowBefore + 1, 'the call reached the server');
  const returned = await run('chat_destroy_now', base, 'chat_destroy_returned');
  assert.equal(returned.returned, true);
  assert.equal(returned.outcomes_before_pump, 0);
  assert.equal(returned.shutdowns_before_pump, 0);
  const ended = await final;
  assert.equal(ended.ok, false);
  assert.equal(ended.kind, 'Cancelled');
  await sleep(50);
  assert.equal((await run('chat_destroy_closed')).shutdowns, 1, 'the last operation shut the transport down');
  for (let i = 0; i < 50 && stats.closed === closedBefore; ++i) await sleep(20);
  assert.equal(stats.closed, closedBefore + 1, 'the server saw the connection close');
});

await check('three calls at once', async () => {
  const report = await run('chat_concurrent');
  assert.equal(report.outcomes, 3);
  assert.equal(report.ok, true, JSON.stringify(report));
});

await check('transport: the request as sent, and nothing delivered inline', async () => {
  const report = await run('direct_echo');
  assert.equal(report.status, 200);
  assert.equal(report.called_before_return, false);
  assert.equal(report.events.split(',').at(0), 'head');
  assert.equal(report.events.split(',').at(-1), 'done');
  assert.equal(report.done_status, 'Completed');
  assert.equal(report.head_seen, true);
  const echoed = JSON.parse(report.body);
  assert.equal(echoed.method, 'POST');
  assert.equal(echoed.headers['content-type'], 'application/json');
  assert.equal(echoed.headers.authorization, 'Bearer sk-not-a-real-key');
  assert.equal(echoed.headers['x-probe'], 'one');
  assert.equal(JSON.parse(echoed.body).probe, '안녕');
});

await check('transport: a redirect is the answer, not followed', async () => {
  const report = await run('direct_redirect');
  assert.equal(report.status, 302);
  assert.equal(report.location, '/echo');
  assert.equal(report.done_status, 'Completed');
  assert.equal(stats.requests.echo, 1, 'only the echo request');
});

await check('transport: a chunk that was not consumed is held until resume', async () => {
  const report = await run('direct_backpressure');
  assert.equal(report.paused_calls, 0, 'nothing is read while paused');
  assert.deepEqual(report.events.split(',').slice(0, 3), ['head', 'refused', 'resume']);
  assert.equal(report.bytes, 8 * 16384);
  assert.equal(report.done_status, 'Completed');
  assert.equal(report.chunks.reduce((sum, size) => sum + size, 0), 8 * 16384);
});

await check('transport: a body larger than the module\'s memory crosses intact', async () => {
  const report = await run('direct_big');
  assert.equal(report.done_status, 'Completed');
  assert.equal(report.bytes, 24 * 1024 * 1024);
  const expected = Array.from({ length: 12 }, (_, i) => (0x30 + i) * 2 * 1024 * 1024).reduce((a, b) => a + b, 0);
  assert.equal(report.sum, expected, 'every byte arrived as sent');
});

await check('transport: a cancelled attempt ends once, Cancelled', async () => {
  const report = await run('direct_abort');
  assert.equal(report.done_status, 'Cancelled');
  assert.equal(report.events.split(',').filter((item) => item === 'done').length, 1);
});

await check('transport: the first-byte bound', async () => {
  const report = await run('direct_first-byte');
  assert.equal(report.done_status, 'Failed');
  assert.equal(report.done_failure, 'FirstByteTimeout');
  assert.equal(report.status, 0);
});

await check('transport: a file body is refused, not inline', async () => {
  const report = await run('direct_files');
  assert.equal(report.done_failure, 'UploadSource');
  assert.equal(report.reached, 'Queued');
  assert.equal(report.called_before_return, false);
});

await check('executor: order, timers, cancellation, batches', async () => {
  const report = await run('executor');
  assert.equal(report.inline, undefined);
  assert.equal(report.order, 'post1,post2,nested,t20,t60');
  assert.equal(report.in_thread_inside, true);
  assert.equal(report.in_thread_outside, false);
  assert.equal(report.host_pumped, true);
  assert.equal(report.batch_ran, 1000);
});

// The example, which wires the seam in a few lines, against the same server (the module path is the optional second argument).
if (process.argv[3]) {
  const createExample = require(path.resolve(process.argv[3]));
  for (const [model, mode, expected] of [['buffered', 'buffer', 'hello from fetch'], ['stream', 'stream', text]]) {
    await check(`example: ${mode}`, async () => {
      const outcome = await new Promise((resolve) => {
        createExample({ onOutcome: (json) => resolve(JSON.parse(json)) }).then((example) => {
          example.ccall('sp_example_chat', null, ['string', 'string', 'string', 'string', 'number'],
                        [base, 'sk-not-a-real-key', model, 'say hello', mode === 'stream' ? 1 : 0]);
        });
      });
      assert.equal(outcome.ok, true, JSON.stringify(outcome));
      assert.equal(outcome.text, expected);
    });
  }
}

server.closeAllConnections();
server.close();
console.log('js host: ok');
process.exit(0);

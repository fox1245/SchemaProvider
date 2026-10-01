// Usage: node differential.mjs <sp_neograph_legacy_case> <tests/fixtures>
// Uses the unchanged v2 corpus and its independent goldens, never golden rewrites.
// The oracle records mismatches but still serves the scripted response, so an
// outbound mismatch cannot hide independent response/parser defects.
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { isDeepStrictEqual } from 'node:util';

const [binary, root] = process.argv.slice(2);
if (!binary || !root) throw new Error('usage: differential.mjs <sp_neograph_legacy_case> <tests/fixtures>');
const fixtures = [];
for (const [family, expected] of [['chat', 35], ['messages', 62]]) {
  const dir = path.join(root, family);
  const entries = fs.readdirSync(dir).filter(x => x.endsWith('.json')).sort();
  if (entries.length !== expected) throw new Error('corpus cardinality changed; review required');
  const descriptor = fs.readFileSync(path.join(root, family === 'chat' ? 'openai-chat-descriptor.json' : 'anthropic-messages-descriptor.json'));
  const identities = new Set();
  for (const name of entries) {
    const f = JSON.parse(fs.readFileSync(path.join(dir, name), 'utf8'));
    if (f.fixture_version !== 2 || f.provenance.kind !== 'synthetic' || f.transport.scheme !== 'http') throw new Error('unsupported corpus provenance');
    if (identities.has(f.case_id)) throw new Error('duplicate fixture identity');
    identities.add(f.case_id);
    const digest = 'sha256:' + crypto.createHash('sha256').update(f.descriptor_source ?? descriptor).digest('hex');
    if (f.descriptor_digest !== digest) throw new Error('fixture descriptor digest mismatch');
    fixtures.push({ ...f, corpus_family: family, directory: dir, comparison_descriptor: JSON.parse(f.descriptor_source ?? descriptor) });
  }
}
const captures = new Map();
const sockets = new Set();
let active;
function differences(expected, actual, prefix = '') {
  if (isDeepStrictEqual(expected, actual)) return [];
  if (expected && actual && typeof expected === 'object' && typeof actual === 'object' && Array.isArray(expected) === Array.isArray(actual)) {
    const keys = new Set([...Object.keys(expected), ...Object.keys(actual)]);
    return [...keys].flatMap(key => differences(expected[key], actual[key], `${prefix}/${key}`));
  }
  return [prefix || '/'];
}
function comparableRequest(f, body) {
  const normalized = structuredClone(body), equivalent = [];
  if (f.request.body.stream === false && normalized.stream === undefined) {
    normalized.stream = false; equivalent.push('omitted-stream-false');
  }
  if (f.corpus_family === 'messages' && Array.isArray(normalized.messages)) {
    for (let i = 0; i < normalized.messages.length; ++i) {
      const wanted = f.request.body.messages[i]?.content;
      const actual = normalized.messages[i]?.content;
      if (typeof actual === 'string' && Array.isArray(wanted) && wanted.length === 1 &&
          isDeepStrictEqual(wanted[0], { type: 'text', text: actual })) {
        normalized.messages[i].content = [{ type: 'text', text: actual }];
        equivalent.push(`plain-text-content:${i}`);
      }
    }
  }
  return { normalized, equivalent };
}
const server = http.createServer(async (req, res) => {
  const state = active;
  if (!state) { req.resume(); res.writeHead(409); res.end(); return; }
  ++state.count;
  const f = state.fixture;
  state.schedule = new Promise(resolve => { state.schedule_finished = resolve; });
  try {
    let bytes = 0;
    const chunks = [];
    for await (const chunk of req) {
      bytes += chunk.length;
      if (bytes > 1 << 20) throw new Error('request bound');
      chunks.push(chunk);
    }
    if (req.method !== f.request.method) state.request.push('method');
    if (req.url !== f.request.path) state.request.push('path');
    if (req.headers.host !== `127.0.0.1:${server.address().port}`) state.request.push('authority');
    for (const [key, value] of Object.entries(f.request.semantic_headers))
      if (req.headers[key.toLowerCase()] !== value) state.request.push(`header:${key.toLowerCase()}`);
    try {
      const actual = JSON.parse(Buffer.concat(chunks).toString('utf8'));
      state.raw_request.push(...differences(f.request.body, actual, 'body'));
      const comparable = comparableRequest(f, actual);
      state.wire_equivalences.push(...comparable.equivalent);
      state.request.push(...differences(f.request.body, comparable.normalized, 'body'));
    } catch { state.request.push('body-json'); }
    const ending = f.transport.schedule.at(-1);
    const how = ending.kind === 'reset' ? 'rst' : ending.how;
    const dataFor = entry => {
      // Current 97 fixtures are inline. Refuse new sidecars rather than silently
      // changing corpus admission or trusting an unchecked filesystem path.
      if (typeof entry.utf8 !== 'string' || entry.sidecar !== undefined) throw new Error('unsupported fixture bytes');
      return Buffer.from(entry.utf8, 'utf8');
    };
    const length = f.transport.schedule.filter(x => x.kind === 'bytes').reduce((n, e) => n + dataFor(e).length, 0);
    const headers = { ...f.transport.headers, Connection: 'close' };
    if (how === 'content_length_met') headers['Content-Length'] = String(length);
    else if (how === 'short_body') headers['Content-Length'] = String(length + 17);
    else if (how === 'chunked_terminator' || how === 'rst') headers['Transfer-Encoding'] = 'chunked';
    else if (how === 'eof_unmarked') res.useChunkedEncodingByDefault = false;
    else throw new Error('unsupported fixture close');
    res.writeHead(f.transport.status, headers);
    res.flushHeaders();
    for (const entry of f.transport.schedule) {
      if (entry.kind === 'bytes') {
        const data = dataFor(entry);
        state.response_bytes += data.length;
        if (!res.write(data)) await Promise.race([once(res, 'drain'), once(res, 'close')]);
      } else if (entry.kind === 'delay_ms') {
        if (!Number.isSafeInteger(entry.value) || entry.value < 0 || entry.value > 1000) throw new Error('invalid fixture delay');
        await new Promise(resolve => setTimeout(resolve, entry.value));
      } else if (entry.kind === 'close' || entry.kind === 'reset') {
        const socket = res.socket;
        if (!socket || socket.destroyed) state.client_closed_before_fault = true;
        else {
          state.close_fired = how;
          if (how === 'rst') socket.resetAndDestroy();
          else if (how === 'short_body' || how === 'eof_unmarked') socket.end();
          else res.end();
        }
      } else throw new Error('unsupported schedule');
    }
  } catch { state.oracle_error = true; res.destroy(); }
  finally { state.schedule_done = true; state.schedule_finished(); }
});
server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
server.on('clientError', (_error, socket) => { if (active) active.request.push('http-framing'); socket.destroy(); });
server.listen(0, '127.0.0.1');
await once(server, 'listening');
const origin = `http://127.0.0.1:${server.address().port}`;
function invoke(fixture) {
  return new Promise((resolve, reject) => {
    const child = spawn(binary, [origin], { stdio: ['pipe', 'pipe', 'pipe'], env: { PATH: process.env.PATH } });
    let output = '', errorBytes = 0;
    const timer = setTimeout(() => child.kill('SIGKILL'), 15000);
    child.on('error', error => { clearTimeout(timer); reject(error); });
    child.stdout.on('data', data => { output += data; if (output.length > 16 << 20) child.kill('SIGKILL'); });
    // Legacy diagnostics can quote raw response content. Discard, never persist.
    child.stderr.on('data', data => { errorBytes += data.length; });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      if (code !== 0 || signal) { reject(new Error('legacy fixture subprocess failed')); return; }
      try { resolve({ ...JSON.parse(output), stderr_emitted: errorBytes !== 0 }); }
      catch { reject(new Error('legacy fixture subprocess emitted invalid report')); }
    });
    child.stdin.on('error', () => {});
    const replay = captures.get(`${fixture.corpus_family}/${fixture.replay_from}`);
    child.stdin.end(JSON.stringify({ fixture, descriptor: fixture.comparison_descriptor, ...(replay ? { replay_message: replay } : {}) }) + '\n');
  });
}
const stopMap = { EndTurn: 'end_turn', ToolUse: 'tool_use', MaxTokens: 'max_tokens', StopSequence: 'stop_sequence', ContentFilter: 'content_filter', Refusal: 'refusal', PauseTurn: 'pause_turn', ContextLimit: 'context_limit', MalformedCall: 'malformed_call', Unknown: 'unknown' };
function compare(f, actual) {
  const semantic = [], unsupported = [...actual.input_gaps], notApplicable = [];
  const expected = f.expect;
  if (expected.outcome !== actual.outcome) semantic.push('terminal-outcome');
  if (expected.outcome === 'failure') {
    // Legacy untyped exceptions cannot prove equivalent classified failure,
    // partial output, stop evidence, retry semantics, or close precedence.
    unsupported.push('typed-error-evidence', 'owned-partial-output');
    if (actual.outcome === 'failure' && actual.class !== 'legacy-untyped' && actual.class !== expected.failure.class)
      semantic.push('failure-class');
  } else if (actual.outcome === 'completion') {
    const expectedCompletion = expected.completion;
    const parts = expectedCompletion.messages.flatMap(m => m.parts);
    const text = parts.filter(p => p.type === 'text').map(p => p.text).join('');
    if (actual.message.content !== text) semantic.push('text-content');
    if (expectedCompletion.messages.length !== 1) unsupported.push('message-cardinality');
    else if (actual.message.role !== expectedCompletion.messages[0].role) semantic.push('message-role');
    const tools = parts.filter(p => (p.type === 'tool_call' || p.type === 'invalid_tool_call') && p.kind === 'ClientExecuted');
    const actualTools = actual.message.tool_calls ?? [];
    if (actualTools.length !== tools.length) semantic.push('tool-cardinality');
    for (let i = 0; i < Math.min(tools.length, actualTools.length); ++i) {
      const tool = actualTools[i], want = tools[i];
      if (tool.id !== want.id || tool.name !== want.name) semantic.push('tool-identity');
      if (want.type !== 'invalid_tool_call') {
        try { if (!isDeepStrictEqual(JSON.parse(tool.arguments), want.input)) semantic.push('tool-input'); }
        catch { semantic.push('tool-json'); }
      }
    }
    if (parts.some(p => p.type === 'invalid_tool_call')) unsupported.push('invalid-tool-kind');
    if (parts.some(p => p.type === 'tool_call' && p.kind !== 'ClientExecuted' || p.type === 'server_tool_result')) unsupported.push('server-tool-authority');
    if (parts.some(p => ['thinking', 'redacted_thinking'].includes(p.type))) unsupported.push('sealed-native-provenance', 'native-part-order');
    if (parts.some(p => p.type === 'refusal')) unsupported.push('refusal-part');
    if (parts.filter(p => p.type === 'text').length > 1 || parts.some((p, i) => p.type === 'text' && parts.slice(0, i).some(q => q.type !== 'text'))) unsupported.push('content-part-order');
    if (actual.stop_reason !== stopMap[expectedCompletion.stop.kind]) semantic.push('stop-kind');
    unsupported.push('raw-stop-evidence', 'usage-provenance');
    if (expectedCompletion.stop.sequence !== undefined || expectedCompletion.stop.details !== undefined) unsupported.push('stop-details');
    const usage = expectedCompletion.usage;
    for (const key of ['input_total', 'output_total', 'total', 'cache_read', 'reasoning']) {
      if (usage[key] === null) unsupported.push('nullable-usage');
      else if (usage[key] !== actual.usage[key]) semantic.push(`usage-${key}`);
    }
    for (const key of ['provider_reported_total', 'input_uncached', 'cache_write'])
      if (usage[key] !== null) unsupported.push(`usage-${key}-representation`);
    if (Object.keys(usage.extra).length) unsupported.push('usage-extra');
  } else if (expected.outcome === 'completion') {
    // Keep the missing expected semantics visible instead of allowing a request
    // mismatch or thrown legacy exception to erase the golden comparison.
    semantic.push('completion-missing');
  }
  if (f.replay_from) unsupported.push('native-origin-binding');
  return { semantic: [...new Set(semantic)].sort(), unsupported: [...new Set(unsupported)].sort(), notApplicable };
}
const counts = { pass: 0, 'expected-fail': 0, unsupported: 0, 'not-applicable': 0 };
let requestMismatchCases = 0, semanticMismatchCases = 0, oracleErrors = 0;
try {
  for (const f of fixtures) {
    active = { fixture: f, count: 0, request: [], raw_request: [], wire_equivalences: [],
      response_bytes: 0, close_fired: '', oracle_error: false, schedule_done: false, client_closed_before_fault: false };
    const actual = await invoke(f);
    const returned_before_close = active.count > 0 && !active.schedule_done;
    if (active.schedule) await active.schedule;
    if (actual.outcome === 'completion') captures.set(`${f.corpus_family}/${f.case_id}`, actual.message);
    if (active.count !== f.expect.request_count) active.request.push('request-count');
    const compared = compare(f, actual);
    if (returned_before_close && actual.outcome === 'completion') compared.semantic.push('early-terminal-before-close');
    const request = [...new Set(active.request)].sort();
    const status = request.length || compared.semantic.length ? 'expected-fail' : compared.unsupported.length ? 'unsupported' : compared.notApplicable.length ? 'not-applicable' : 'pass';
    ++counts[status];
    if (request.length) ++requestMismatchCases;
    if (compared.semantic.length) ++semanticMismatchCases;
    if (active.oracle_error || (active.count && !active.close_fired && !active.client_closed_before_fault)) ++oracleErrors;
    process.stdout.write(JSON.stringify({ family: f.corpus_family, case_id: f.case_id, status,
      independently_accepted: false, expected_fail_is: 'observed-defect-candidate-not-an-admission',
      request_mismatches: request, semantic_mismatches: compared.semantic,
      unsupported: compared.unsupported, not_applicable: compared.notApplicable,
      raw_body_mismatches: [...new Set(active.raw_request)].sort(), wire_equivalences: [...new Set(active.wire_equivalences)].sort(),
      returned_before_close, client_closed_before_fault: active.client_closed_before_fault,
      defect_classes: [...new Set([...request.map(() => 'outbound-contract'), ...compared.semantic])],
      request_count: active.count, response_bytes: active.response_bytes, close_fired: active.close_fired,
      oracle_error: active.oracle_error, legacy_stderr_suppressed: actual.stderr_emitted }) + '\n');
  }
  const equivalent = counts.pass === fixtures.length && oracleErrors === 0;
  process.stdout.write(JSON.stringify({ summary: counts, fixtures: fixtures.length, request_mismatch_cases: requestMismatchCases,
    semantic_mismatch_cases: semanticMismatchCases, oracle_errors: oracleErrors, equivalent,
    lossless_cutover: equivalent ? 'go' : 'no-go', independent_expected_fail_acceptance: false }) + '\n');
  process.exitCode = equivalent ? 0 : 1;
} finally {
  active = undefined;
  server.close();
  for (const socket of sockets) socket.destroy();
}

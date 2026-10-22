// Records real provider responses for the replay mode of the SDK comparison.
//
//   node record.mjs --api openai|anthropic --mode buffered|stream --model ID --text FILE --out FILE
//                   [--count 30] [--max-attempts 45] [--pause-ms 1000] [--reasoning-effort none]
//
// Each request asks the model to copy one fixed text (FILE, read verbatim) and nothing else, so that every kept
// recording carries the same reply text and the benchmark clients can verify it unchanged. A response whose text
// differs from FILE in any byte is not kept; the count of such responses is part of the output.
//
// What is recorded per response: the status, an allowlist of headers, the time from the moment the request was fully
// handed to the socket to the response head, and every piece of the body as it arrived (offset in microseconds and the
// bytes, base64). Identifiers in the body and the request-id headers are replaced by synthetic ones of the same length,
// so the byte offsets of the pieces stay valid. Bodies are requested with `accept-encoding: identity`.
//
// The key is read from OPENAI_API_KEY or ANTHROPIC_API_KEY and never written anywhere. Uses Node's https (HTTP/1.1),
// the same protocol the peer serves in the benchmark.
import https from 'node:https';
import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';

const argv = process.argv.slice(2);
const opt = (name, fallback) => { const i = argv.indexOf(name); return i >= 0 ? argv[i + 1] : fallback; };
const api = opt('--api'), mode = opt('--mode'), model = opt('--model'), out = opt('--out');
const textFile = opt('--text');
const want = Number(opt('--count', '30')), maxAttempts = Number(opt('--max-attempts', '45')), pauseMs = Number(opt('--pause-ms', '1000'));
const effort = opt('--reasoning-effort');
if (!['openai', 'anthropic'].includes(api) || !['buffered', 'stream'].includes(mode) || !model || !out || !textFile) {
  process.stderr.write('usage: record.mjs --api openai|anthropic --mode buffered|stream --model ID --text FILE --out FILE\n');
  process.exit(2);
}
const text = readFileSync(textFile, 'utf8');
const stream = mode === 'stream';
const system = 'You are a benchmark assistant.';
const prompt = 'Reply with exactly the text between <copy> and </copy>, without the tags, without quotes, and with nothing before or after it.\n'
  + `<copy>${text}</copy>`;
const maxTokens = 1024;

function request() {
  if (api === 'anthropic') {
    const key = process.env.ANTHROPIC_API_KEY;
    if (!key) throw new Error('ANTHROPIC_API_KEY is not set');
    const body = { model, max_tokens: maxTokens, system, messages: [{ role: 'user', content: prompt }] };
    if (stream) body.stream = true;
    return { host: 'api.anthropic.com', path: '/v1/messages', body,
      headers: { 'x-api-key': key, 'anthropic-version': '2023-06-01' } };
  }
  const key = process.env.OPENAI_API_KEY;
  if (!key) throw new Error('OPENAI_API_KEY is not set');
  const body = { model, max_completion_tokens: maxTokens, messages: [{ role: 'system', content: system }, { role: 'user', content: prompt }] };
  if (effort) body.reasoning_effort = effort;
  if (stream) { body.stream = true; body.stream_options = { include_usage: true }; }
  return { host: 'api.openai.com', path: '/v1/chat/completions', body, headers: { authorization: `Bearer ${key}` } };
}

const KEEP = new Set(['content-type', 'content-length', 'cache-control', 'request-id', 'x-request-id', 'openai-processing-ms', 'openai-version']);
const SYNTHETIC_ID = new Set(['request-id', 'x-request-id']);
const synthetic = (prefix, length) => (prefix + 'bench' + '0'.repeat(length)).slice(0, length);

function once() {
  const r = request();
  const payload = Buffer.from(JSON.stringify(r.body));
  return new Promise((resolve, reject) => {
    let t0, head;
    const pieces = [];
    const req = https.request({ host: r.host, path: r.path, method: 'POST', agent: false,
      headers: { ...r.headers, 'content-type': 'application/json', 'content-length': payload.length, 'accept-encoding': 'identity' } }, res => {
      head = process.hrtime.bigint();
      res.on('data', chunk => pieces.push([process.hrtime.bigint(), chunk]));
      res.on('end', () => resolve({ t0, head, status: res.statusCode, rawHeaders: res.rawHeaders, pieces }));
      res.on('error', reject);
    });
    req.on('error', reject);
    req.on('finish', () => { t0 = process.hrtime.bigint(); });
    req.end(payload);
  });
}

function replyText(status, body) {
  if (status !== 200) return { text: undefined, deltas: 0 };
  const s = body.toString('utf8');
  if (!stream) {
    const j = JSON.parse(s);
    return { text: api === 'anthropic' ? j.content?.filter(p => p.type === 'text').map(p => p.text).join('') : j.choices?.[0]?.message?.content, deltas: 0 };
  }
  let out = '', deltas = 0;
  for (const line of s.split('\n')) {
    if (!line.startsWith('data:')) continue;
    const data = line.slice(5).trim();
    if (!data || data === '[DONE]') continue;
    const j = JSON.parse(data);
    const piece = api === 'anthropic'
      ? (j.type === 'content_block_delta' && j.delta?.type === 'text_delta' ? j.delta.text : undefined)
      : j.choices?.[0]?.delta?.content;
    if (typeof piece === 'string' && piece.length) { out += piece; ++deltas; }
  }
  return { text: out, deltas };
}

const scrub = s => s.replace(/(chatcmpl-|msg_|req_)[A-Za-z0-9]+/g, (m, prefix) => synthetic(prefix, m.length));
// Replace identifiers on the whole body, then cut it back at the original piece boundaries (same-length replacements).
function sanitize(pieces) {
  const whole = Buffer.concat(pieces.map(([, b]) => b)).toString('latin1')
    .replace(/chatcmpl-[A-Za-z0-9]+/g, m => synthetic('chatcmpl-', m.length))
    .replace(/msg_[A-Za-z0-9]+/g, m => synthetic('msg_', m.length))
    .replace(/req_[A-Za-z0-9]+/g, m => synthetic('req_', m.length));
  const bytes = Buffer.from(whole, 'latin1');
  const outPieces = []; let at = 0;
  for (const [, b] of pieces) { outPieces.push(bytes.subarray(at, at + b.length)); at += b.length; }
  return outPieces;
}

const us = (a, b) => Number((b - a) / 1000n);
const recordings = [];
const rejected = [];
let attempts = 0;
while (recordings.length < want && attempts < maxAttempts) {
  ++attempts;
  let r;
  try { r = await once(); } catch (e) { rejected.push({ reason: scrub(`transport: ${e.message}`) }); continue; }
  const body = Buffer.concat(r.pieces.map(([, b]) => b));
  let parsed;
  try { parsed = replyText(r.status, body); } catch (e) { parsed = { text: undefined, deltas: 0, error: e.message }; }
  if (r.status !== 200 || parsed.text !== text) {
    const reason = r.status !== 200 ? `status ${r.status}: ${body.toString('utf8').slice(0, 300)}`
      : `text differs (${parsed.text === undefined ? 'none' : parsed.text.length + ' chars'})`;
    rejected.push({ reason: scrub(reason) });
    process.stderr.write(`attempt ${attempts}: rejected, ${scrub(reason)}\n`);
    if (r.status === 400 || r.status === 401 || r.status === 404) break;  // a request problem; more attempts would only repeat it
  } else {
    const headers = [];
    for (let i = 0; i < r.rawHeaders.length; i += 2) {
      const name = r.rawHeaders[i].toLowerCase();
      if (!KEEP.has(name)) continue;
      headers.push([name, SYNTHETIC_ID.has(name) ? synthetic('req_', r.rawHeaders[i + 1].length) : r.rawHeaders[i + 1]]);
    }
    const clean = sanitize(r.pieces);
    recordings.push({
      status: r.status, headers, head_us: us(r.t0, r.head),
      chunks: r.pieces.map(([t], i) => [us(r.t0, t), clean[i].toString('base64')]),
      body_bytes: body.length, deltas: parsed.deltas,
    });
    process.stderr.write(`attempt ${attempts}: kept ${recordings.length}/${want}, head ${us(r.t0, r.head) / 1000} ms, end ${us(r.t0, r.pieces.at(-1)[0]) / 1000} ms, ${r.pieces.length} pieces, ${parsed.deltas} deltas\n`);
  }
  if (pauseMs) await new Promise(res => setTimeout(res, pauseMs));
}

const file = {
  version: 1, api, mode, model, recorded_utc: new Date().toISOString().replace(/\.\d+Z$/, 'Z'),
  text_sha256: createHash('sha256').update(text, 'utf8').digest('hex'), text_bytes: Buffer.byteLength(text),
  request: { max_tokens: maxTokens, reasoning_effort: effort ?? null, accept_encoding: 'identity', http: '1.1' },
  attempts, kept: recordings.length, rejected, recordings,
};
writeFileSync(out, JSON.stringify(file) + '\n');
process.stderr.write(`${out}: kept ${recordings.length} of ${attempts} attempts\n`);
process.exit(recordings.length === want ? 0 : 1);

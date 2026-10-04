// Native Gemini model-free oracle. Synthetic fixture pixels, no model API calls.
import http from 'node:http';
import readline from 'node:readline';
import { createHash } from 'node:crypto';
import { isDeepStrictEqual as equal } from 'node:util';
import { fixtureScenes, question } from '../../tools/vision_fixture.mjs';

const key = 'GEMINI_SYNTHETIC_KEY';
const scenes = fixtureScenes();
const cases = new Map(), sockets = new Set(), waiters = new Set();
let unexpected = 0;
const reply = value => process.stdout.write(JSON.stringify(value) + '\n');
const notify = () => { for (const wake of [...waiters]) wake(); };
const snapshot = c => ({ count: c.count, invalid: c.invalid, unexpected, faults: c.faults,
  held: c.held.size, closed: c.closed, replayed: c.replayed, images: c.images });
const usage = { promptTokenCount: 10, cachedContentTokenCount: 4, candidatesTokenCount: 7,
  thoughtsTokenCount: 3, totalTokenCount: 20, toolUsePromptTokenCount: 2 };
function group(scene) {
  return [{ thought: true, text: 'Use the image and the lookup result', thoughtSignature: 'THOUGHT_SIG' },
    { text: 'need lookup', thoughtSignature: 'TEXT_SIG' },
    { functionCall: { id: 'call_owned', name: 'lookup', args: { x: scene.expected.weighted } }, thoughtSignature: 'CALL_SIG' },
    { thoughtSignature: 'EMPTY_SIG' }];
}
const frame = data => `data: ${JSON.stringify(data)}\n\n`;
function response(model, parts, reason, counters = usage, id = 'generation') {
  const r = { modelVersion: model, responseId: id, candidates: [{ index: 0, content: { role: 'model', parts } }] };
  if (reason) r.candidates[0].finishReason = reason;
  if (counters !== undefined) r.usageMetadata = counters;
  return r;
}
function sse(model, parts, reason = 'STOP', counters = usage) {
  return parts.map(p => frame(response(model, [p], undefined, undefined))).join('')
    + frame(response(model, [], reason, counters));
}
function examine(req, body, model, c) {
  if (req.method !== 'POST' || req.headers['x-goog-api-key'] !== key || req.headers.authorization || req.headers['x-api-key']) throw new Error('auth');
  if (req.url !== `/v1beta/models/${model}:generateContent` && req.url !== `/v1beta/models/${model}:streamGenerateContent?alt=sse`) throw new Error('path');
  if (!equal(body.systemInstruction, { parts: [{ text: 'Answer briefly' }] })) throw new Error('system');
  const portable = c.scenario === 'portable' || c.scenario === 'portable-text';
  const settings = [
    { category: 'HARM_CATEGORY_HARASSMENT', threshold: 'BLOCK_NONE' },
    { category: 'HARM_CATEGORY_HATE_SPEECH', threshold: 'BLOCK_ONLY_HIGH' },
    { category: 'HARM_CATEGORY_SEXUALLY_EXPLICIT', threshold: 'BLOCK_MEDIUM_AND_ABOVE' },
    { category: 'HARM_CATEGORY_DANGEROUS_CONTENT', threshold: 'BLOCK_LOW_AND_ABOVE' },
    { category: 'HARM_CATEGORY_CIVIC_INTEGRITY', threshold: 'OFF' }];
  if (!equal(body.generationConfig, portable
    ? { maxOutputTokens: 2048, temperature: 0.25, thinkingConfig: { includeThoughts: false, thinkingLevel: 'high' } }
    : { maxOutputTokens: 2048, thinkingConfig: { includeThoughts: true, thinkingBudget: 1024 } })) throw new Error('thinking');
  if (portable && (!equal(body.safetySettings, settings) ||
      !equal(body.toolConfig, { functionCallingConfig: { mode: 'VALIDATED', allowedFunctionNames: ['lookup'] } }))) throw new Error('controls');
  const tool = body.tools?.[0]?.functionDeclarations?.[0];
  if (!equal(tool, { name: 'lookup', description: 'Find value', parametersJsonSchema: { type: 'object', properties: { x: { type: 'integer' } } } })) throw new Error('function');
  const prefix = body.contents?.[0];
  if (prefix?.role !== 'user' || prefix.parts?.length !== 2 || prefix.parts[0].text !== question || prefix.parts[1].inlineData?.mimeType !== 'image/png') throw new Error('image order');
  const data = prefix.parts[1].inlineData.data;
  const bytes = Buffer.from(data, 'base64'), hash = createHash('sha256').update(bytes).digest('hex');
  const scene = scenes.find(s => s.sha256 === hash && s.png.equals(bytes));
  if (!scene || bytes.toString('base64') !== data) throw new Error('image bytes');
  if (c.scenario === 'scene-b' && scene.name !== 'scene-b') throw new Error('changed image');
  ++c.images;
  if (c.count === 1) { c.prefix = prefix; c.scene = scene; }
  else if (!portable) {
    const tr = body.contents?.[2]?.parts?.[0]?.functionResponse;
    if (!equal(prefix, c.prefix) || !equal(body.contents?.[1], { role: 'model', parts: group(c.scene) })
      || !equal(tr, { name: 'lookup', id: 'call_owned', response: { result: c.scene.expected.weighted } }) || body.contents.length !== 3) throw new Error('replay ownership');
    ++c.replayed;
  }
  if (portable) {
    const foreignText = { role: 'model', parts: [{ text: 'foreign answer' }] };
    if (!equal(body.contents?.[1], foreignText)) throw new Error('foreign text');
    let expected = [prefix, foreignText];
    if (c.scenario === 'portable') {
      expected.push({ role: 'model', parts: [
        { text: 'checking' },
        { functionCall: { id: 'foreign-a', name: 'lookup', args: { x: 1 } }, thoughtSignature: 'skip_thought_signature_validator' },
        { functionCall: { id: 'foreign-b', name: 'lookup', args: { x: 2 } } }] });
      expected.push({ role: 'user', parts: [
        { functionResponse: { name: 'lookup', id: 'foreign-b', response: { result: 2 } } },
        { functionResponse: { name: 'lookup', id: 'foreign-a', response: { result: 1 } } }] });
    }
    if (c.count > 1) expected.push({ role: 'model', parts: [{ text: 'portable accepted', thoughtSignature: 'NEW_NATIVE_SIG' }] });
    if (!equal(body.contents, expected) || !equal(prefix, c.prefix)) throw new Error('portable ownership');
    if (c.count > 1) ++c.replayed;
  }
  return scene;
}
const server = http.createServer(async (req, res) => {
  let c;
  try {
    const chunks = []; let bytes = 0;
    for await (const chunk of req) { bytes += chunk.length; if (bytes > (1 << 20)) throw new Error('bound'); chunks.push(chunk); }
    const body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    const model = /^\/v1beta\/models\/([^/]+):/.exec(req.url)?.[1];
    c = cases.get(model);
    if (!c) { ++unexpected; res.writeHead(400); res.end(); return; }
    ++c.count;
    let scene;
    try { scene = examine(req, body, model, c); } catch { ++c.invalid; res.writeHead(400); res.end(); notify(); return; }
    const streaming = req.url.includes(':streamGenerateContent');
    const fault = () => { ++c.faults; notify(); };
    res.on('close', () => { ++c.closed; c.held.delete(res); notify(); });
    if (c.scenario === 'reset') { fault(); res.socket.resetAndDestroy(); return; }
    let parts = c.count === 1 ? group(scene) : [{ text: JSON.stringify({ result: scene.expected.weighted }) }];
    if (['close-gate', 'completed-reset', 'completed-error', 'short-close', 'hold'].includes(c.scenario)) parts = [{ thought: true, text: 'Thinking', thoughtSignature: 'THOUGHT_SIG' }, { text: 'hello' }];
    if (c.scenario === 'invalid-args') parts = [{ functionCall: { id: 'bad', name: 'lookup', args: [] } }];
    if (c.scenario === 'portable' || c.scenario === 'portable-text')
      parts = [{ text: 'portable accepted', thoughtSignature: 'NEW_NATIVE_SIG' }];
    const counters = c.scenario === 'nullable-usage' ? { promptTokenCount: 10, candidatesTokenCount: 7, thoughtsTokenCount: null, totalTokenCount: 20 } : usage;
    const data = streaming ? sse(model, parts, 'STOP', counters) : JSON.stringify(response(model, parts, 'STOP', counters));
    const headers = { 'Content-Type': streaming ? 'text/event-stream' : 'application/json', Connection: 'close' };
    if (c.scenario === 'short-close') headers['Content-Length'] = Buffer.byteLength(data) + 23;
    res.writeHead(200, headers); res.flushHeaders();
    if (c.scenario === 'hold') { c.held.set(res, () => res.end(data)); notify(); return; }
    if (c.scenario === 'partial' || c.scenario === 'partial-error') {
      res.write(frame(response(model, [{ text: 'partial owned' }], undefined, undefined)));
      if (c.scenario === 'partial-error') { fault(); res.end(frame({ error: { message: key + 'THOUGHT_SIG' } })); }
      else { c.held.set(res, () => { fault(); res.destroy(); }); notify(); }
      return;
    }
    res.write(data);
    if (c.scenario === 'completed-error') { fault(); res.end(frame({ error: { message: key + 'THOUGHT_SIG' } })); return; }
    if (c.scenario === 'close-gate' || c.scenario === 'completed-reset') {
      c.held.set(res, () => { if (c.scenario === 'completed-reset') { fault(); res.destroy(); } else res.end(); }); notify(); return;
    }
    if (c.scenario === 'short-close') { fault(); res.socket.end(); return; }
    res.end(); notify();
  } catch { if (c) ++c.invalid; else ++unexpected; res.destroy(); notify(); }
});
server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
server.on('clientError', (_error, socket) => socket.destroy());
server.listen(0, '127.0.0.1', () => reply({ port: server.address().port }));
const control = readline.createInterface({ input: process.stdin });
control.on('line', async line => {
  try {
    const command = JSON.parse(line);
    if (command.arm) {
      if (cases.has(command.arm)) throw new Error('duplicate');
      cases.set(command.arm, { scenario: command.scenario, count: 0, invalid: 0, faults: 0, held: new Map(), closed: 0, replayed: 0, images: 0 });
      reply({ armed: true }); return;
    }
    const c = cases.get(command.model); if (!c) throw new Error('unknown');
    if (command.wait) await new Promise((resolve, reject) => {
      const ready = () => { const stats = snapshot(c); if (Object.entries(command.wait).every(([key, value]) => stats[key] >= value)) { clearTimeout(timer); waiters.delete(ready); resolve(); } };
      const timer = setTimeout(() => { waiters.delete(ready); reject(new Error('timeout')); }, 12000);
      waiters.add(ready); ready();
    });
    if (command.release) { const held = [...c.held.values()]; c.held.clear(); for (const release of held) release(); notify(); }
    reply(snapshot(c));
  } catch { reply({ error: 'Gemini peer control failure' }); }
});
control.on('close', () => { server.close(); for (const socket of sockets) socket.destroy(); });

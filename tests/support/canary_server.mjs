// Private model-free oracle. Credentials, requests and native leaves stay in memory.
import http from 'node:http';
import readline from 'node:readline';
const cases = new Map();
const sockets = new Set();
const secret = 'CANARY_SECRET_KEY_MARKER';
const signature = 'CANARY_SIGNATURE_MARKER';
const thinking = 'CANARY_THINKING_MARKER';
const redacted = 'CANARY_REDACTED_MARKER';
const text = 'CANARY_RESPONSE_MARKER';
let unexpected = 0;
const reply = value => process.stdout.write(`${JSON.stringify(value)}\n`);
const frame = (type, value, messages) => `${messages ? `event: ${type}\n` : ''}data: ${JSON.stringify(value)}\n\n`;
const send = (res, code, body, stream = false) => {
  res.writeHead(code, { 'Content-Type': stream ? 'text/event-stream' : 'application/json',
    'Content-Length': Buffer.byteLength(body), Connection: 'close' });
  res.end(body);
};
function message(model, content, tool = false) {
  return { id: 'CANARY_ACCOUNT_ID_MARKER', type: 'message', role: 'assistant', model, content,
    stop_reason: tool ? 'tool_use' : 'end_turn', stop_sequence: null, usage: { input_tokens: 2, output_tokens: 3,
      ...(cases.get(model)?.scenario === 'known-usage' ? { cache_read_input_tokens: 0, cache_creation_input_tokens: 0 } : {}) } };
}
function chat(model, tool = false) {
  return { id: 'CANARY_ACCOUNT_ID_MARKER', object: 'chat.completion', created: 1, model,
    choices: [{ index: 0, message: { role: 'assistant', content: tool ? null : text,
      ...(tool ? { tool_calls: [{ id: 'canary_call', type: 'function', function: { name: 'canary_echo', arguments: '{"value":7}' } }] } : {}) },
    finish_reason: tool ? 'tool_calls' : 'stop' }], usage: { prompt_tokens: 2, completion_tokens: 3, total_tokens: 5,
      ...(cases.get(model)?.scenario === 'inconsistent-usage' ? { prompt_tokens_details: { cached_tokens: 3 } } : {}) } };
}
function sse(model, messages, includeUsage) {
  if (!messages) return frame('', { id: 'canary-chat', object: 'chat.completion.chunk', created: 1, model,
    choices: [{ index: 0, delta: { role: 'assistant', content: text }, finish_reason: null }] }, false)
    + frame('', { id: 'canary-chat', object: 'chat.completion.chunk', created: 1, model,
      choices: [{ index: 0, delta: {}, finish_reason: 'stop' }],
      ...(includeUsage ? { usage: { prompt_tokens: 2, completion_tokens: 3, total_tokens: 5 } } : {}) }, false)
    + 'data: [DONE]\n\n';
  return frame('message_start', { type: 'message_start', message: { ...message(model, []), stop_reason: null, usage: { input_tokens: 2, output_tokens: 0 } } }, true)
    + frame('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } }, true)
    + frame('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text } }, true)
    + frame('content_block_stop', { type: 'content_block_stop', index: 0 }, true)
    + frame('message_delta', { type: 'message_delta', delta: { stop_reason: 'end_turn', stop_sequence: null }, usage: { output_tokens: 3 } }, true)
    + frame('message_stop', { type: 'message_stop' }, true);
}
const server = http.createServer(async (req, res) => {
  let c;
  try {
    const chunks = []; let size = 0;
    for await (const chunk of req) { size += chunk.length; if (size > (1 << 20)) throw Error(); chunks.push(chunk); }
    const raw = Buffer.concat(chunks).toString('utf8');
    const body = JSON.parse(raw); c = cases.get(body.model);
    if (!c) { ++unexpected; send(res, 400, '{}'); return; }
    ++c.count;
    const messages = req.url === '/v1/messages';
    const gemini = req.url === '/v1beta/openai/chat/completions';
    if (req.method !== 'POST' || (!messages && !gemini && req.url !== '/v1/chat/completions') ||
        (messages ? req.headers['x-api-key'] !== secret || req.headers['anthropic-version'] !== '2023-06-01'
          : req.headers.authorization !== `Bearer ${secret}`) ||
        typeof body.stream !== 'boolean' || !Array.isArray(body.messages) ||
        body.max_tokens !== (gemini ? 128 : 2048) || req.headers['anthropic-beta'] || body.cache_control ||
        (messages || gemini ? body.service_tier !== undefined : body.service_tier !== 'default') ||
        (gemini && (body.reasoning_effort !== 'none' || body.tools || body.extra_body || req.headers['x-api-key'] ||
                    req.headers['anthropic-version']))) ++c.invalid;
    if (c.scenario === 'remote-error') {
      ++c.faults; send(res, 503, JSON.stringify({ type: 'error', error: { type: 'api_error', message: secret + thinking + text } })); return;
    }
    if (c.scenario === 'gemini-auth-error') {
      ++c.faults;
      send(res, 403, JSON.stringify({ error: { code: 403, status: 'PERMISSION_DENIED', message: secret + text } }));
      return;
    }
    if (body.stream) {
      ++c.sse; send(res, 200, sse(body.model, messages, body.stream_options?.include_usage === true), true); return;
    }
    const tools = Array.isArray(body.tools) && body.tools.length > 0;
    if (tools && (body.tools.length !== 1 || (messages ? body.tools[0].name : body.tools[0].function?.name) !== 'canary_echo' ||
        (messages && (body.thinking?.type !== 'enabled' || body.thinking?.budget_tokens !== 1024)))) ++c.invalid;
    if (tools && body.messages.length === 1) {
      const native = c.scenario === 'absent' ? [{ type: 'thinking', thinking }]
        : [{ type: 'thinking', thinking, signature }, { type: 'redacted_thinking', data: redacted }];
      c.original = [...native, { type: 'tool_use', id: 'canary_call', name: 'canary_echo', input: { value: 7 } }];
      send(res, 200, JSON.stringify(messages ? message(body.model, c.original, true) : chat(body.model, true))); return;
    }
    if (tools) {
      if (body.messages.length !== 3) ++c.invalid;
      if (!messages) {
        if (body.messages[1].role !== 'assistant' || body.messages[1].tool_calls?.[0]?.id !== 'canary_call' ||
            body.messages[2].role !== 'tool' || body.messages[2].tool_call_id !== 'canary_call' || body.messages[2].content !== '{"value":7}') ++c.invalid;
        ++c.positive;
      } else {
        const content = body.messages[1].content;
        const result = body.messages[2].content?.[0];
        if (body.messages[1].role !== 'assistant' || body.messages[2].role !== 'user' || result?.type !== 'tool_result' ||
            result.tool_use_id !== 'canary_call' || result.content !== '{"value":7}' || result.is_error !== false) ++c.invalid;
        if (JSON.stringify(content) === JSON.stringify(c.original)) {
          ++c.positive; c.positiveWire = raw; ++c.retained;
        } else {
          ++c.negative;
          let differences = Math.abs(raw.length - (c.positiveWire?.length ?? 0));
          for (let i = 0; i < Math.min(raw.length, c.positiveWire?.length ?? 0); ++i) if (raw[i] !== c.positiveWire[i]) ++differences;
          c.differences += differences;
          const changed = structuredClone(c.original); changed[0].signature = 'A' + signature.slice(1);
          if (differences !== 1 || JSON.stringify(content) !== JSON.stringify(changed)) ++c.invalid;
          if (c.scenario !== 'accept') {
            ++c.faults;
            const diagnostic = c.scenario === 'misleading' ? 'Invalid tool_use_id; signature verification was not attempted'
              : c.scenario === 'unrelated' ? 'invalid unrelated request ' + text : 'Invalid signature in thinking block.';
            send(res, 400, JSON.stringify({ type: 'error', error: { type: 'invalid_request_error', message: diagnostic } })); return;
          }
        }
      }
    }
    if (c.scenario === 'gemini-corrupt') {
      const invalid = chat(body.model); delete invalid.created; ++c.faults;
      send(res, 200, JSON.stringify(invalid)); return;
    }
    send(res, 200, JSON.stringify(messages ? message(body.model, [{ type: 'text', text }]) : chat(body.model)));
  } catch { if (c) ++c.invalid; else ++unexpected; if (!res.headersSent) send(res, 500, '{}'); else res.destroy(); }
});
server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
server.listen(0, '127.0.0.1', () => reply({ port: server.address().port }));
const input = readline.createInterface({ input: process.stdin });
input.on('line', line => {
  try {
    const command = JSON.parse(line);
    if (command.arm) {
      cases.set(command.arm, { scenario: command.scenario, count: 0, invalid: 0, faults: 0, positive: 0, negative: 0, retained: 0, differences: 0, sse: 0 });
      reply({ ok: true });
    } else {
      const c = cases.get(command.model); if (!c) throw Error();
      reply({ count: c.count, invalid: c.invalid, faults: c.faults, positive: c.positive, negative: c.negative,
        retained: c.retained, differences: c.differences, sse: c.sse, unexpected });
    }
  } catch { reply({ error: 'invalid control' }); }
});
input.on('close', () => { for (const socket of sockets) socket.destroy(); server.close(); });

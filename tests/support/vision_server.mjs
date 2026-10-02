// Synthetic HTTP/SSE oracle. No model API, credentials, or external images.
import http from 'node:http';
import readline from 'node:readline';
import { createHash } from 'node:crypto';
import { isDeepStrictEqual as equal } from 'node:util';
import { fixtureScenes, question } from '../../tools/vision_fixture.mjs';

const key = 'VISION_SYNTHETIC_KEY';
const signature = 'VISION_PRIVATE_SIGNATURE';
const scenes = fixtureScenes();
const cases = new Map(), sockets = new Set(), waiters = new Set();
let unexpected = 0;
const reply = value => process.stdout.write(JSON.stringify(value) + '\n');
const notify = () => { for (const wake of [...waiters]) wake(); };
const snapshot = c => ({ count: c.count, invalid: c.invalid, unexpected, faults: c.faults,
  held: c.held.size, closed: c.closed, images: c.images, replayed: c.replayed, on: c.on, off: c.off, sse: c.sse,
  ...(c.scenario === 'campaign' ? { retained: c.retained, signature_negative: c.signatureNegative,
    reasoning_missing: c.reasoningMissing, scene_a: c.sceneA, scene_b: c.sceneB, tool_first: c.toolFirst } : {}) });
const scoreSchema = { type: 'object', properties: { value: { type: 'integer' } }, required: ['value'], additionalProperties: false };
const frame = (event, data) => `${event ? `event: ${event}\n` : ''}data: ${JSON.stringify(data)}\n\n`;
const schema = { type: 'object', properties: { red_circles: { type: 'integer' }, blue_squares: { type: 'integer' }, weighted: { type: 'integer' } }, required: ['red_circles', 'blue_squares', 'weighted'], additionalProperties: false };
function withoutSignatures(value) {
  if (Array.isArray(value)) return value.map(withoutSignatures);
  if (!value || typeof value !== 'object') return value;
  return Object.fromEntries(Object.entries(value).filter(([key]) => !['signature', 'thoughtSignature', 'encrypted_content'].includes(key)).map(([key, field]) => [key, withoutSignatures(field)]));
}
function signatureDifferences(expected, incoming) {
  if (typeof expected === 'string' && typeof incoming === 'string') {
    if (expected.length !== incoming.length) return Infinity;
    return [...expected].reduce((count, byte, index) => count + (byte !== incoming[index]), 0);
  }
  if (Array.isArray(expected) && Array.isArray(incoming) && expected.length === incoming.length)
    return expected.reduce((count, value, index) => count + signatureDifferences(value, incoming[index]), 0);
  if (expected && incoming && typeof expected === 'object' && typeof incoming === 'object' &&
      equal(Object.keys(expected).sort(), Object.keys(incoming).sort()))
    return Object.keys(expected).reduce((count, key) => count + signatureDifferences(expected[key], incoming[key]), 0);
  return equal(expected, incoming) ? 0 : Infinity;
}
function nativeMatches(c, incoming, family) {
  if (equal(c.native, incoming)) {
    if (c.scenario === 'campaign') ++c.retained;
    return true;
  }
  if (c.scenario !== 'campaign' || !Array.isArray(incoming)) return false;
  const remaining = c.native.filter(part => family === 'responses' ? part.type !== 'reasoning' :
    family === 'messages' ? !['thinking', 'redacted_thinking'].includes(part.type) :
    family === 'gemini' ? part.thought !== true : part.type !== 'thought').map(part =>
      family === 'gemini' ? Object.fromEntries(Object.entries(part).filter(([key]) => key !== 'thoughtSignature')) :
      family === 'interactions' && part.type === 'function_call' ? Object.fromEntries(Object.entries(part).filter(([key]) => key !== 'signature')) : part);
  if (remaining.length < c.native.length && equal(remaining, incoming)) {
    ++c.reasoningMissing;
    if (c.acceptOmission) return true;
    const error = new Error('native reasoning omitted'); error.signature = true; error.omission = true; throw error;
  }
  if (equal(withoutSignatures(c.native), withoutSignatures(incoming)) &&
      signatureDifferences(c.native, incoming) === 1) {
    const carrierKey = family === 'gemini' ? 'thoughtSignature' : 'signature';
    const callIndex = c.native.findIndex(part => (family === 'gemini' ? part.functionCall : family === 'interactions' && part.type === 'function_call') && typeof part[carrierKey] === 'string');
    if (callIndex >= 0 && c.native[callIndex][carrierKey] === incoming[callIndex]?.[carrierKey]) return false;
    ++c.signatureNegative;
    if (c.acceptSignature) return true;
    const error = new Error('native signature mismatch'); error.signature = true; throw error;
  }
  return false;
}
function signatureError(family) {
  if (family === 'responses') return { type: 'invalid_request_error', code: 'invalid_encrypted_content', message: 'Invalid encrypted content' };
  if (family === 'messages') return { type: 'invalid_request_error', message: 'Invalid signature in thinking block' };
  if (family === 'interactions') return { code: 'invalid_request', message: 'Invalid thought signature' };
  return { code: 400, status: 'INVALID_ARGUMENT', message: 'Invalid thought signature' };
}
function familyOf(req) {
  if (req.url === '/v1/chat/completions') return 'chat';
  if (req.url === '/v1/responses') return 'responses';
  if (req.url === '/v1/messages') return 'messages';
  if (req.url === '/v1beta/interactions') return 'interactions';
  if (/^\/v1beta\/models\/[^/]+:(streamGenerateContent\?alt=sse|generateContent)$/.test(req.url)) return 'gemini';
  throw new Error('endpoint');
}
function examine(req, body, c, family) {
  const auth = family === 'messages' ? req.headers['x-api-key'] : family === 'gemini' || family === 'interactions' ? req.headers['x-goog-api-key'] : req.headers.authorization?.replace(/^Bearer /, '');
  if (req.method !== 'POST' || auth !== key) throw new Error('auth');
  const content = family === 'responses' ? body.input?.[0]?.content : family === 'interactions' ? body.input?.[0]?.content : family === 'gemini' ? body.contents?.[0]?.parts : body.messages?.[0]?.content;
  const campaign = c.scenario === 'campaign';
  if (!Array.isArray(content) || (content.length !== 2 && !(campaign && family !== 'chat' && content.length === 3))) throw new Error('ordered content');
  // Both interfaces have an exact order contract, not an unordered content search.
  const imageFirst = family === 'chat' || c.scenario === 'campaign';
  const image = content[imageFirst ? 0 : 1], text = content[imageFirst ? 1 : 0];
  const toolQuestion = 'Call vision_score exactly once with value equal to weighted. After tool result return the same three-field JSON.';
  if (text.text !== question && !(campaign && family === 'chat' && text.text === question + '\n' + toolQuestion)) throw new Error('question');
  if (content.length === 3 && content[2]?.text !== toolQuestion) throw new Error('tool question order');
  if (campaign) {
    const cap = family === 'chat' ? body.max_completion_tokens ?? body.max_tokens : family === 'responses' ? body.max_output_tokens : family === 'messages' ? body.max_tokens : family === 'gemini' ? body.generationConfig?.maxOutputTokens : body.generation_config?.max_output_tokens;
    if (cap !== 8192) throw new Error('campaign output cap');
  }
  let mime, data;
  if (family === 'chat' || family === 'responses') {
    const uri = family === 'chat' ? image.image_url?.url : image.image_url;
    if (image.type !== (family === 'chat' ? 'image_url' : 'input_image')) throw new Error('image type');
    const match = /^data:([^;,]+);base64,([A-Za-z0-9+/]+={0,2})$/.exec(uri ?? '');
    if (!match) throw new Error('image URI');
    [, mime, data] = match;
  } else if (family === 'messages') {
    if (image.type !== 'image' || image.source?.type !== 'base64') throw new Error('image source');
    mime = image.source.media_type; data = image.source.data;
  } else if (family === 'gemini') { mime = image.inlineData?.mimeType; data = image.inlineData?.data; }
  else { if (image.type !== 'image') throw new Error('image type'); mime = image.mime_type; data = image.data; }
  if (mime !== 'image/png' || typeof data !== 'string') throw new Error('MIME');
  const bytes = Buffer.from(data, 'base64');
  if (bytes.toString('base64') !== data) throw new Error('noncanonical image');
  const hash = createHash('sha256').update(bytes).digest('hex');
  const scene = scenes.find(s => s.sha256 === hash && s.png.equals(bytes));
  if (!scene) throw new Error('image bytes');
  if (c.scenario.includes('vision-b') && scene.name !== 'scene-b') throw new Error('changed image');
  if (c.scenario.includes('vision-a') && scene.name !== 'scene-a') throw new Error('first image');
  ++c.images;
  if (campaign) ++c[scene.name === 'scene-a' ? 'sceneA' : 'sceneB'];
  const control = family === 'chat' ? body.reasoning_effort : family === 'responses' ? body.reasoning?.effort : family === 'messages' ? body.thinking?.budget_tokens : family === 'gemini' ? body.generationConfig?.thinkingConfig?.thinkingBudget : body.generation_config?.thinking_level;
  const on = family === 'messages' || family === 'gemini' ? control === 1024 :
    family === 'responses' ? control === 'low' || control === 'medium' :
    control === (campaign && family === 'interactions' ? 'high' : 'low');
  const off = family === 'messages' ? body.thinking === undefined : family === 'gemini' ? control === 0 : family === 'interactions' ? control === undefined && body.generation_config?.thinking_summaries === 'none' : control === 'none';
  if (!on && !off) throw new Error('reasoning control');
  if (c.scenario.includes('-off') && !off) throw new Error('off control');
  if (c.scenario.includes('-on') && !on) throw new Error('on control');
  if (family === 'gemini' && (typeof body.generationConfig?.thinkingConfig?.includeThoughts !== 'boolean' || body.generationConfig.thinkingConfig.includeThoughts !== on)) throw new Error('thought visibility');
  if (family === 'interactions' && body.generation_config?.thinking_summaries !== (on ? 'auto' : 'none')) throw new Error('summary visibility');
  if (campaign && family === 'responses' && body.reasoning?.summary !== 'auto') throw new Error('summary visibility');
  ++c[on ? 'on' : 'off'];
  const tools = family === 'gemini' ? body.tools?.[0]?.functionDeclarations : body.tools;
  const tool = tools?.[0];
  const toolName = tool?.name ?? tool?.function?.name;
  if (tool && toolName !== 'observe_scene' && !(c.scenario === 'campaign' && toolName === 'vision_score')) throw new Error('tool');
  if (tool && !equal(family === 'chat' ? tool.function?.parameters : family === 'messages' ? tool.input_schema : family === 'gemini' ? tool.parametersJsonSchema : tool.parameters, toolName === 'vision_score' ? scoreSchema : schema)) throw new Error('tool schema');
  if (family === 'responses' && body.store !== false || family === 'interactions' && body.store !== false) throw new Error('store');
  if (c.native) {
    const answer = JSON.stringify(c.toolPayload);
    if (scene.sha256 !== c.scene.sha256) throw new Error('replay image prefix');
    if (family === 'responses') {
      if (body.input.at(-1)?.call_id !== 'scene_call' || body.input.at(-1)?.output !== answer || !nativeMatches(c, body.input.slice(1, -1), family)) throw new Error('response replay');
    } else if (family === 'messages') {
      if (body.messages[2]?.content?.[0]?.tool_use_id !== 'scene_call' || body.messages[2]?.content?.[0]?.content !== answer || !nativeMatches(c, body.messages[1]?.content, family)) throw new Error('message replay');
    } else if (family === 'gemini') {
      const result = body.contents[2]?.parts?.[0]?.functionResponse;
      if (result?.id !== 'scene_call' || result.name !== c.toolName || !equal(result.response, c.toolPayload) || !nativeMatches(c, body.contents[1]?.parts, family)) throw new Error('gemini replay');
    } else if (family === 'interactions') {
      const result = body.input.at(-1);
      if (result?.type !== 'function_result' || result.call_id !== 'scene_call' || result.name !== c.toolName || result.result !== answer || !nativeMatches(c, body.input.slice(1, -1), family)) throw new Error('interaction replay');
    } else if (body.messages[1]?.tool_calls?.[0]?.id !== 'scene_call' || body.messages[2]?.tool_call_id !== 'scene_call' || body.messages[2]?.content !== answer) throw new Error('chat loop');
    ++c.replayed;
  }
  if (campaign && tool && !c.native) {
    if (scene.name !== 'scene-a' || scene.expected.weighted !== 8) throw new Error('campaign first tool image');
    ++c.toolFirst;
  }
  return { scene, on, tool: !!tool && !c.native, toolName: toolName ?? 'observe_scene' };
}
function wire(family, model, scene, on, tool, missing, toolName = 'observe_scene', presentation = false) {
  const toolPayload = toolName === 'vision_score' ? { value: scene.expected.weighted } : scene.expected;
  const jsonAnswer = JSON.stringify(scene.expected);
  const answer = presentation ? '```json\n' + jsonAnswer + '\n```' : jsonAnswer;
  const argumentsJson = presentation && family === 'chat' && toolName === 'vision_score' ?
    '{"value":' + scene.expected.weighted + '.0}' : JSON.stringify(toolPayload);
  const call = { id: 'scene_call', name: toolName };
  const summary = 'Count colored shapes, then compute the weighted value.';
  let native;
  if (family === 'chat') {
    const message = tool ? { role: 'assistant', content: null, tool_calls: [{ id: call.id, type: 'function', function: { name: call.name, arguments: argumentsJson } }] } : { role: 'assistant', content: answer };
    const usage = missing ? undefined : { prompt_tokens: 10, completion_tokens: 7, total_tokens: 17, completion_tokens_details: { reasoning_tokens: on ? 3 : 0 } };
    const base = { id: 'vision_chat', object: 'chat.completion', created: 1, model };
    const delta = tool ? { ...message, tool_calls: message.tool_calls.map((call, index) => ({ index, ...call })) } : message;
    return { native: message, buffered: { ...base, choices: [{ index: 0, message, finish_reason: tool ? 'tool_calls' : 'stop' }], ...(usage ? { usage } : {}) }, sse: frame('', { ...base, object: 'chat.completion.chunk', choices: [{ index: 0, delta, finish_reason: null }] }) + frame('', { ...base, object: 'chat.completion.chunk', choices: [{ index: 0, delta: {}, finish_reason: tool ? 'tool_calls' : 'stop' }], ...(usage ? { usage } : {}) }) + 'data: [DONE]\n\n', partial: frame('', { ...base, object: 'chat.completion.chunk', choices: [{ index: 0, delta: { role: 'assistant', content: answer }, finish_reason: null }] }) };
  }
  if (family === 'messages') {
    native = [...(on ? [{ type: 'thinking', thinking: summary, signature }] : []), tool ? { type: 'tool_use', ...call, input: toolPayload } : { type: 'text', text: answer }];
    const usage = missing ? { input_tokens: 10, output_tokens: 7 } : { input_tokens: 10, output_tokens: 7, cache_read_input_tokens: 0, cache_creation_input_tokens: 0 };
    const message = { id: 'vision_message', type: 'message', role: 'assistant', model, content: native, stop_reason: tool ? 'tool_use' : 'end_turn', stop_sequence: null, ...(usage ? { usage } : {}) };
    let sse = frame('message_start', { type: 'message_start', message: { ...message, content: [], stop_reason: null } });
    native.forEach((part, index) => {
      const start = part.type === 'tool_use' ? { ...part, input: {} } : part.type === 'thinking' ? { type: 'thinking', thinking: '', signature: '' } : { type: 'text', text: '' };
      sse += frame('content_block_start', { type: 'content_block_start', index, content_block: start });
      const delta = part.type === 'tool_use' ? { type: 'input_json_delta', partial_json: argumentsJson } : part.type === 'thinking' ? { type: 'thinking_delta', thinking: summary } : { type: 'text_delta', text: answer };
      sse += frame('content_block_delta', { type: 'content_block_delta', index, delta });
      if (part.type === 'thinking') sse += frame('content_block_delta', { type: 'content_block_delta', index, delta: { type: 'signature_delta', signature } });
      sse += frame('content_block_stop', { type: 'content_block_stop', index });
    });
    const terminal = { type: 'message_delta', delta: { stop_reason: message.stop_reason, stop_sequence: null }, ...(usage ? { usage } : {}) };
    sse += frame('message_delta', terminal) + frame('message_stop', { type: 'message_stop' });
    const partial = frame('message_start', { type: 'message_start', message: { ...message, content: [], stop_reason: null } }) + frame('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } }) + frame('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: answer } });
    return { native, buffered: message, sse, partial };
  }
  if (family === 'responses') {
    native = [...(on ? [{ id: 'scene_reasoning', type: 'reasoning', status: 'completed', summary: [{ type: 'summary_text', text: summary }], encrypted_content: signature }] : []), tool ? { id: 'scene_fc', type: 'function_call', status: 'completed', call_id: call.id, name: call.name, arguments: argumentsJson } : { id: 'scene_msg', type: 'message', status: 'completed', role: 'assistant', content: [{ type: 'output_text', text: answer, annotations: [] }] }];
    const usage = missing ? null : { input_tokens: 10, output_tokens: 7, total_tokens: 17, output_tokens_details: { reasoning_tokens: on ? 3 : 0 } };
    const buffered = { id: 'vision_response', object: 'response', created_at: 1, model, status: 'completed', output: native, usage, error: null };
    let sse = frame('response.created', { type: 'response.created', response: { ...buffered, output: [], status: 'in_progress', usage: null } });
    native.forEach((item, output_index) => {
      sse += frame('response.output_item.added', { type: 'response.output_item.added', output_index, item: item.type === 'function_call' ? { ...item, arguments: '', status: 'in_progress' } : item.type === 'reasoning' ? { id: item.id, type: item.type, summary: [] } : { ...item, content: [], status: 'in_progress' } });
      if (item.type === 'function_call') sse += frame('response.function_call_arguments.delta', { type: 'response.function_call_arguments.delta', output_index, item_id: item.id, delta: argumentsJson });
      if (item.type === 'message' || item.type === 'reasoning') {
        const reason = item.type === 'reasoning', part = reason ? item.summary[0] : item.content[0], owner = { output_index, item_id: item.id, [reason ? 'summary_index' : 'content_index']: 0 };
        sse += frame(reason ? 'response.reasoning_summary_part.added' : 'response.content_part.added', { type: reason ? 'response.reasoning_summary_part.added' : 'response.content_part.added', ...owner, part: { ...part, text: '' } });
        sse += frame(reason ? 'response.reasoning_summary_text.delta' : 'response.output_text.delta', { type: reason ? 'response.reasoning_summary_text.delta' : 'response.output_text.delta', ...owner, delta: part.text });
      }
      sse += frame('response.output_item.done', { type: 'response.output_item.done', output_index, item });
    });
    sse += frame('response.completed', { type: 'response.completed', response: buffered });
    const item = { id: 'scene_msg', type: 'message', status: 'in_progress', role: 'assistant', content: [] };
    const partial = frame('response.created', { type: 'response.created', response: { ...buffered, output: [], status: 'in_progress', usage: null } }) + frame('response.output_item.added', { type: 'response.output_item.added', output_index: 0, item }) + frame('response.content_part.added', { type: 'response.content_part.added', output_index: 0, item_id: item.id, content_index: 0, part: { type: 'output_text', text: '', annotations: [] } }) + frame('response.output_text.delta', { type: 'response.output_text.delta', output_index: 0, item_id: item.id, content_index: 0, delta: answer });
    return { native, buffered, sse, partial };
  }
  if (family === 'gemini') {
    native = [...(on ? [{ text: summary, thought: true, thoughtSignature: signature }] : []), tool ? { functionCall: { ...call, args: toolPayload }, ...(on ? { thoughtSignature: 'VISION_FUNCTION_SIGNATURE' } : {}) } : { text: answer }];
    const usageMetadata = missing ? undefined : { promptTokenCount: 10, candidatesTokenCount: 4, thoughtsTokenCount: on ? 3 : 0, totalTokenCount: on ? 17 : 14, cachedContentTokenCount: 0 };
    const buffered = { candidates: [{ index: 0, content: { role: 'model', parts: native }, finishReason: 'STOP' }], modelVersion: model, responseId: 'vision_generation', ...(usageMetadata ? { usageMetadata } : {}) };
    const partial = frame('', { candidates: [{ index: 0, content: { role: 'model', parts: [{ text: answer }] } }], modelVersion: model, responseId: 'vision_generation' });
    return { native, buffered, sse: frame('', buffered), partial };
  }
  native = [...(on ? [{ type: 'thought', signature, summary: [{ type: 'text', text: summary }] }] : []), tool ? { type: 'function_call', ...call, arguments: toolPayload, ...(on && presentation ? { signature } : {}) } : { type: 'model_output', content: [{ type: 'text', text: answer }] }];
  const usage = missing ? undefined : { total_input_tokens: 10, total_output_tokens: 4, total_thought_tokens: on ? 3 : 0, total_tokens: on ? 17 : 14, total_cached_tokens: 0, total_tool_use_tokens: 0 };
  const buffered = { id: 'vision_interaction', model, status: tool ? 'requires_action' : 'completed', steps: native, ...(usage ? { usage } : {}) };
  let sse = frame('interaction.created', { event_type: 'interaction.created', interaction: { id: buffered.id, model, status: 'in_progress' } });
  native.forEach((step, index) => {
    sse += frame('step.start', { event_type: 'step.start', index, step: step.type === 'thought' ? { type: 'thought', summary: [] } : step.type === 'function_call' ? { ...step, arguments: {} } : { type: 'model_output', content: [] } });
    sse += frame('step.delta', { event_type: 'step.delta', index, delta: step.type === 'function_call' ? { type: 'arguments_delta', arguments: argumentsJson } : step.type === 'thought' ? { type: 'thought_summary', content: { type: 'text', text: summary } } : { type: 'text', text: answer } });
    if (step.type === 'thought') sse += frame('step.delta', { event_type: 'step.delta', index, delta: { type: 'thought_signature', signature } });
    sse += frame('step.stop', { event_type: 'step.stop', index });
  });
  sse += frame('interaction.completed', { event_type: 'interaction.completed', interaction: buffered });
  const partial = frame('interaction.created', { event_type: 'interaction.created', interaction: { id: buffered.id, model, status: 'in_progress' } }) + frame('step.start', { event_type: 'step.start', index: 0, step: { type: 'model_output', content: [] } }) + frame('step.delta', { event_type: 'step.delta', index: 0, delta: { type: 'text', text: answer } });
  return { native, buffered, sse, partial };
}
const server = http.createServer(async (req, res) => {
  let c;
  try {
    const chunks = []; let length = 0;
    for await (const chunk of req) { length += chunk.length; if (length > 1 << 20) throw new Error('bound'); chunks.push(chunk); }
    const body = JSON.parse(Buffer.concat(chunks).toString('utf8')), family = familyOf(req);
    const model = family === 'gemini' ? decodeURIComponent(req.url.split('/models/')[1].split(':')[0]) : body.model;
    c = cases.get(model); if (!c) { ++unexpected; res.writeHead(400); res.end(); return; }
    ++c.count;
    let observed;
    try { observed = examine(req, body, c, family); } catch (error) {
      if (error.signature) {
        const control = error.omission ? c.omissionError : c.signatureError;
        const data = control?.body ?? JSON.stringify({ error: signatureError(family) });
        const headers = { 'Content-Type': control?.content_type ?? 'application/json', Connection: 'close' };
        if (control?.short_close) headers['Content-Length'] = Buffer.byteLength(data) + 37;
        ++c.faults; res.writeHead(control?.status ?? 400, headers);
        if (control?.short_close) { res.write(data); res.socket.end(); }
        else res.end(data);
      } else { ++c.invalid; res.writeHead(400); res.end(JSON.stringify({ error: { message: 'invalid synthetic request' } })); }
      notify(); return;
    }
    res.on('close', () => { ++c.closed; c.held.delete(res); notify(); });
    const streaming = family === 'gemini' ? req.url.includes('streamGenerateContent') : body.stream === true;
    if (streaming) ++c.sse;
    if (c.scenario === 'replay-rejected' && c.count > 1) {
      ++c.faults; res.writeHead(400, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: { type: 'invalid_request_error', code: 'invalid_signature', message: signature + key } })); notify(); return;
    }
    const oracleScene = c.scenario === 'campaign' && c.wrongOracle && c.count === 1 ?
      { ...observed.scene, expected: { ...observed.scene.expected, weighted: observed.scene.expected.weighted + 1 } } : observed.scene;
    const output = wire(family, model, oracleScene, observed.on, observed.tool, c.scenario === 'missing-usage', observed.toolName, c.scenario === 'campaign');
    if (observed.tool) { c.native = output.native; c.scene = observed.scene; c.toolName = observed.toolName; c.toolPayload = observed.toolName === 'vision_score' ? { value: observed.scene.expected.weighted } : observed.scene.expected; }
    const data = streaming ? output.sse : JSON.stringify(output.buffered);
    const headers = { 'Content-Type': streaming ? 'text/event-stream' : 'application/json', Connection: 'close' };
    if (c.scenario === 'short-close') headers['Content-Length'] = Buffer.byteLength(data) + 37;
    res.writeHead(200, headers); res.flushHeaders();
    if (c.scenario === 'partial-error') { res.write(output.partial); c.held.set(res, () => { ++c.faults; res.destroy(); notify(); }); notify(); return; }
    res.write(data);
    if (c.scenario === 'close-gate') { c.held.set(res, () => res.end()); notify(); return; }
    if (c.scenario === 'short-close') { ++c.faults; res.socket.end(); notify(); return; }
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
      cases.set(command.arm, { scenario: command.scenario, count: 0, invalid: 0, faults: 0, held: new Map(), closed: 0, images: 0, replayed: 0, on: 0, off: 0, sse: 0,
        retained: 0, signatureNegative: 0, reasoningMissing: 0, sceneA: 0, sceneB: 0, toolFirst: 0,
        wrongOracle: command.scenario === 'campaign' && command.wrong_oracle === true,
        signatureError: command.signature_error, omissionError: command.omission_error,
        acceptSignature: command.accept_signature === true, acceptOmission: command.accept_omission === true }); reply({ armed: true }); return;
    }
    const c = cases.get(command.model); if (!c) throw new Error('unknown');
    if (command.wait) await new Promise((resolve, reject) => {
      const ready = () => { const stats = snapshot(c); if (Object.entries(command.wait).every(([name, count]) => stats[name] >= count)) { clearTimeout(timer); waiters.delete(ready); resolve(); } };
      const timer = setTimeout(() => { waiters.delete(ready); reject(new Error('timeout')); }, 12000); waiters.add(ready); ready();
    });
    if (command.release) { const held = [...c.held.values()]; c.held.clear(); for (const release of held) release(); notify(); }
    reply(snapshot(c));
  } catch { reply({ error: 'vision peer control failure' }); }
});
control.on('close', () => { server.close(); for (const socket of sockets) socket.destroy(); });

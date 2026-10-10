// Loopback oracle for typed media over the five wire families (Chat, Responses, Messages, Gemini
// generateContent, Gemini Interactions). It owns the fixture bytes, validates every received request
// body against the vendor wire shape independently of the SDK (payloads are checked by SHA-256 of the
// base64-decoded bytes), and answers with model-generated media in buffered and SSE form, with
// deliberately awkward TCP write boundaries and injectable payload/transport faults.
// Control protocol: one JSON object per stdin line, one JSON reply per stdout line.
import http from 'node:http';
import readline from 'node:readline';
import { createHash } from 'node:crypto';
import { isDeepStrictEqual } from 'node:util';

const KEYS = { chat: 'MEDIA_SYNTHETIC_KEY', responses: 'MEDIA_SYNTHETIC_KEY', messages: 'MEDIA_SYNTHETIC_KEY',
  gemini: 'MEDIA_SYNTHETIC_KEY', interactions: 'MEDIA_SYNTHETIC_KEY' };
const cases = new Map(), sockets = new Set();
let unexpected = 0;
const reply = value => process.stdout.write(JSON.stringify(value) + '\n');
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');

// ---------------------------------------------------------------------------------------------
// Fixtures: tiny deterministic payloads whose first bytes are the real magic numbers.
// ---------------------------------------------------------------------------------------------
const u32 = n => { const b = Buffer.alloc(4); b.writeUInt32BE(n >>> 0); return b; };
const le32 = n => { const b = Buffer.alloc(4); b.writeUInt32LE(n >>> 0); return b; };
const asc = text => Buffer.from(text, 'latin1');
const ftyp = (brand, ...compat) => Buffer.concat([u32(16 + 4 * compat.length), asc('ftyp'), asc(brand), u32(0), ...compat.map(asc)]);
const FIXTURES = {
  png: ['image/png', 'image', () => Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), u32(13), asc('IHDR')])],
  jpeg: ['image/jpeg', 'image', () => Buffer.from([0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00])],
  webp: ['image/webp', 'image', () => Buffer.concat([asc('RIFF'), le32(4096), asc('WEBPVP8 ')])],
  gif: ['image/gif', 'image', () => asc('GIF89a')],
  heic: ['image/heic', 'image', () => ftyp('heic', 'mif1', 'heic')],
  heif: ['image/heif', 'image', () => ftyp('mif1', 'mif1', 'heic')],
  bmp: ['image/bmp', 'image', () => asc('BM')],
  pdf: ['application/pdf', 'document', () => asc('%PDF-1.7\n%\xe2\xe3\xcf\xd3\n1 0 obj\n')],
  txt: ['text/plain', 'document', () => asc('plain text document body\n')],
  csv: ['text/csv', 'document', () => asc('a,b\n1,2\n')],
  wav: ['audio/wav', 'audio', () => Buffer.concat([asc('RIFF'), le32(4096), asc('WAVEfmt '), le32(16), Buffer.from([1, 0, 1, 0, 0x80, 0x3e, 0, 0])])],
  mp3: ['audio/mp3', 'audio', () => Buffer.concat([asc('ID3'), Buffer.from([4, 0, 0, 0, 0, 0, 0x7f]), Buffer.from([0xff, 0xfb, 0x90, 0x00])])],
  mpeg: ['audio/mpeg', 'audio', () => Buffer.from([0xff, 0xfb, 0x90, 0x64])],
  flac: ['audio/flac', 'audio', () => asc('fLaC')],
  ogg: ['audio/ogg', 'audio', () => asc('OggS\0\x02')],
  l16: ['audio/l16', 'audio', () => Buffer.alloc(0)],
  mp4: ['video/mp4', 'video', () => ftyp('isom', 'isom', 'mp41')],
  webm: ['video/webm', 'video', () => Buffer.from([0x1a, 0x45, 0xdf, 0xa3])],
  mov: ['video/mov', 'video', () => ftyp('qt  ', 'qt  ')],
};
const bySha = new Map();
function makeFixture(name, size = 256, salt = 0, mime) {
  const spec = FIXTURES[name];
  if (!spec) throw new Error('fixture');
  const head = spec[2]();
  const body = Buffer.alloc(Math.max(0, size - head.length));
  let x = (Math.imul(salt + 1, 0x9e3779b1) ^ Math.imul(size, 0x85ebca6b) ^ name.length) >>> 0 || 1;
  for (let i = 0; i < body.length; ++i) { x ^= x << 13; x >>>= 0; x ^= x >>> 17; x ^= x << 5; x >>>= 0; body[i] = x & 255; }
  const bytes = Buffer.concat([head, body]);
  const fixture = { name, kind: spec[1], mime: mime ?? spec[0], bytes, b64: bytes.toString('base64'), sha256: sha256(bytes) };
  bySha.set(fixture.sha256, fixture);
  return fixture;
}

// ---------------------------------------------------------------------------------------------
// Request-side oracle: normalize each family's media blocks from the raw JSON, strictly.
// ---------------------------------------------------------------------------------------------
const bad = message => { throw new Error(message); };
const keysIn = (object, allowed, where) => {
  if (object === null || typeof object !== 'object' || Array.isArray(object)) bad(`${where}: object required`);
  for (const key of Object.keys(object)) if (!allowed.includes(key)) bad(`${where}: unexpected key ${key}`);
};
function canonical(b64, where) {
  if (typeof b64 !== 'string' || b64.length === 0) bad(`${where}: base64 string required`);
  const bytes = Buffer.from(b64, 'base64');
  if (bytes.toString('base64') !== b64) bad(`${where}: base64 is not canonical`);
  return bytes;
}
function inlineItem(kind, mime, b64, where, extra = {}) {
  const bytes = canonical(b64, where);
  const sha = sha256(bytes);
  const fixture = bySha.get(sha);
  if (!fixture || !fixture.bytes.equals(bytes)) bad(`${where}: payload is not a known fixture`);
  return { t: 'media', kind, mime, source: 'inline', sha256: sha, ...extra };
}
function dataUri(text, where) {
  const match = /^data:([A-Za-z0-9.+-]+\/[A-Za-z0-9.+-]+(?:;[A-Za-z0-9._-]+=[A-Za-z0-9._-]+)*);base64,([A-Za-z0-9+/]+={0,2})$/.exec(text);
  if (!match) bad(`${where}: data URI required`);
  return { mime: match[1], b64: match[2] };
}
const https = (text, where) => {
  if (typeof text !== 'string' || !/^https:\/\/[^\s"'\\]+$/.test(text)) bad(`${where}: https URL required`);
  return text;
};
// Short wire format names are not MIME types; each maps to the MIME spellings that denote them.
const FORMAT_MIMES = { wav: ['audio/wav'], mp3: ['audio/mp3', 'audio/mpeg'], aiff: ['audio/aiff'], aac: ['audio/aac'], ogg: ['audio/ogg'],
  flac: ['audio/flac'], m4a: ['audio/m4a'], pcm16: ['audio/l16'], pcm24: ['audio/l24'] };
const DETAILS = ['auto', 'low', 'high', 'original'];
function imageUrlItem(url, where, detail) {
  if (detail !== undefined && !DETAILS.includes(detail)) bad(`${where}: detail`);
  if (url.startsWith('data:')) { const d = dataUri(url, where); return inlineItem('image', d.mime, d.b64, where); }
  return { t: 'media', kind: 'image', source: 'url', ref: https(url, where) };
}
function fileLocation(kind, text, name, where) {
  if (text.startsWith('data:')) { const d = dataUri(text, where); return inlineItem(kind, d.mime, d.b64, where, name ? { name } : {}); }
  return { t: 'media', kind, source: 'url', ref: https(text, where) };
}

function chatItems(body) {
  const user = body.messages?.filter(m => m.role === 'user').at(-1);
  if (!user) bad('chat: user message');
  if (typeof user.content === 'string') return [{ t: 'text', text: user.content }];
  const items = [];
  for (const [i, part] of user.content.entries()) {
    const where = `chat content[${i}]`;
    if (part.type === 'text') { keysIn(part, ['type', 'text'], where); items.push({ t: 'text', text: part.text }); }
    else if (part.type === 'image_url') {
      keysIn(part, ['type', 'image_url'], where); keysIn(part.image_url, ['url', 'detail'], where);
      items.push(imageUrlItem(part.image_url.url, where, part.image_url.detail));
    } else if (part.type === 'input_audio') {
      keysIn(part, ['type', 'input_audio'], where); keysIn(part.input_audio, ['data', 'format'], where);
      const format = part.input_audio.format;
      if (!(format in FORMAT_MIMES)) bad(`${where}: audio format`);
      // data is raw base64, never a data URI.
      items.push(inlineItem('audio', undefined, part.input_audio.data, where, { format }));
    } else if (part.type === 'video_url') {
      keysIn(part, ['type', 'video_url'], where); keysIn(part.video_url, ['url', 'processing'], where);
      const url = part.video_url.url;
      if (url.startsWith('data:')) { const d = dataUri(url, where); items.push(inlineItem('video', d.mime, d.b64, where)); }
      else items.push({ t: 'media', kind: 'video', source: 'url', ref: https(url, where) });
    } else if (part.type === 'file') {
      keysIn(part, ['type', 'file'], where); keysIn(part.file, ['file_data', 'file_id', 'filename'], where);
      const f = part.file;
      if (f.file_id !== undefined) {
        if (f.file_data !== undefined || typeof f.file_id !== 'string' || !f.file_id) bad(`${where}: exactly one of file_id/file_data`);
        items.push({ t: 'media', kind: 'document', source: 'file', ref: f.file_id });
      } else {
        if (typeof f.filename !== 'string' || !f.filename) bad(`${where}: filename required with file_data`);
        items.push(fileLocation('document', f.file_data, f.filename, where));
      }
    } else bad(`${where}: unknown part type ${part.type}`);
  }
  return items;
}
function responsesItems(body) {
  const items = [];
  const inputs = Array.isArray(body.input) ? body.input : bad('responses: input array');
  const message = inputs.filter(m => (m.type === undefined || m.type === 'message') && m.role === 'user').at(-1);
  if (!message) bad('responses: user message');
  if (typeof message.content === 'string') return [{ t: 'text', text: message.content }];
  for (const [i, part] of message.content.entries()) {
    const where = `responses content[${i}]`;
    if (part.type === 'input_text') { keysIn(part, ['type', 'text'], where); items.push({ t: 'text', text: part.text }); }
    else if (part.type === 'input_image') {
      keysIn(part, ['type', 'image_url', 'file_id', 'detail'], where);
      if (!DETAILS.includes(part.detail)) bad(`${where}: detail`);
      if ((part.image_url === undefined) === (part.file_id === undefined)) bad(`${where}: exactly one of image_url/file_id`);
      items.push(part.file_id !== undefined ? { t: 'media', kind: 'image', source: 'file', ref: part.file_id } : imageUrlItem(part.image_url, where));
    } else if (part.type === 'input_file') {
      keysIn(part, ['type', 'file_id', 'file_url', 'file_data', 'filename', 'detail'], where);
      if (part.file_id !== undefined) items.push({ t: 'media', kind: 'document', source: 'file', ref: part.file_id });
      else if (part.file_url !== undefined) items.push({ t: 'media', kind: 'document', source: 'url', ref: https(part.file_url, where) });
      else {
        if (typeof part.filename !== 'string' || !part.filename) bad(`${where}: filename required with file_data`);
        const d = dataUri(part.file_data, where);
        items.push(inlineItem('document', d.mime, d.b64, where, { name: part.filename }));
      }
    } else if (part.type === 'input_audio') {
      keysIn(part, ['type', 'input_audio'], where); keysIn(part.input_audio, ['data', 'format'], where);
      if (!['wav', 'mp3'].includes(part.input_audio.format)) bad(`${where}: Responses audio format is wav or mp3`);
      items.push(inlineItem('audio', undefined, part.input_audio.data, where, { format: part.input_audio.format }));
    } else if (part.type === 'input_video') {
      keysIn(part, ['type', 'video_url', 'processing'], where);
      if (typeof part.video_url !== 'string') bad(`${where}: video_url is a string`);
      if (part.video_url.startsWith('data:')) { const d = dataUri(part.video_url, where); items.push(inlineItem('video', d.mime, d.b64, where)); }
      else items.push({ t: 'media', kind: 'video', source: 'url', ref: https(part.video_url, where) });
    } else bad(`${where}: unknown part type ${part.type}`);
  }
  return items;
}
function messagesItems(body) {
  const message = body.messages?.filter(m => m.role === 'user').at(-1);
  if (!message) bad('messages: user message');
  if (typeof message.content === 'string') return [{ t: 'text', text: message.content }];
  const items = [];
  for (const [i, block] of message.content.entries()) {
    const where = `messages content[${i}]`;
    if (block.type === 'text') { keysIn(block, ['type', 'text'], where); items.push({ t: 'text', text: block.text }); continue; }
    if (block.type !== 'image' && block.type !== 'document') bad(`${where}: block type ${block.type}`);
    keysIn(block, ['type', 'source', 'title', 'context', 'citations', 'cache_control'], where);
    const kind = block.type === 'image' ? 'image' : 'document', s = block.source;
    const extra = block.title !== undefined ? { name: block.title } : {};
    if (s.type === 'base64') {
      keysIn(s, ['type', 'media_type', 'data'], where);
      items.push(inlineItem(kind, s.media_type, s.data, where, extra));
    } else if (s.type === 'text') {
      // PlainTextSource carries the text itself, not base64.
      keysIn(s, ['type', 'media_type', 'data'], where);
      if (kind !== 'document' || s.media_type !== 'text/plain' || typeof s.data !== 'string') bad(`${where}: text source`);
      const bytes = Buffer.from(s.data, 'utf8'), sha = sha256(bytes), fixture = bySha.get(sha);
      if (!fixture || !fixture.bytes.equals(bytes)) bad(`${where}: text payload is not a known fixture`);
      items.push({ t: 'media', kind, mime: 'text/plain', source: 'inline', sha256: sha, ...extra });
    } else if (s.type === 'url') { keysIn(s, ['type', 'url'], where); items.push({ t: 'media', kind, source: 'url', ref: https(s.url, where), ...extra }); }
    else if (s.type === 'file') { keysIn(s, ['type', 'file_id'], where); items.push({ t: 'media', kind, source: 'file', ref: s.file_id, ...extra }); }
    else bad(`${where}: source type ${s.type}`);
  }
  return items;
}
function geminiItems(body) {
  const content = body.contents?.filter(c => c.role === 'user').at(-1) ?? bad('gemini: user content');
  const items = [];
  for (const [i, part] of content.parts.entries()) {
    const where = `gemini parts[${i}]`;
    if ('text' in part) { keysIn(part, ['text'], where); items.push({ t: 'text', text: part.text }); }
    else if ('inlineData' in part) {
      keysIn(part, ['inlineData'], where); keysIn(part.inlineData, ['mimeType', 'data', 'displayName'], where);
      if (typeof part.inlineData.mimeType !== 'string') bad(`${where}: mimeType`);
      items.push(inlineItem(undefined, part.inlineData.mimeType, part.inlineData.data, where));
    } else if ('fileData' in part) {
      keysIn(part, ['fileData'], where); keysIn(part.fileData, ['mimeType', 'fileUri', 'displayName'], where);
      items.push({ t: 'media', kind: undefined, mime: part.fileData.mimeType, source: 'ref', ref: https(part.fileData.fileUri, where) });
    } else bad(`${where}: unknown part`);
  }
  return items;
}
function interactionsItems(body) {
  const step = body.input?.filter(s => s.type === 'user_input').at(-1) ?? bad('interactions: user_input step');
  const items = [];
  for (const [i, c] of step.content.entries()) {
    const where = `interactions content[${i}]`;
    if (c.type === 'text') { keysIn(c, ['type', 'text', 'annotations'], where); items.push({ t: 'text', text: c.text }); continue; }
    if (!['image', 'audio', 'video', 'document'].includes(c.type)) bad(`${where}: content type ${c.type}`);
    keysIn(c, ['type', 'mime_type', 'data', 'uri', 'resolution', 'channels', 'sample_rate', 'processing', 'name'], where);
    if ((c.data === undefined) === (c.uri === undefined)) bad(`${where}: exactly one of data/uri`);
    if (c.data !== undefined) {
      if (typeof c.mime_type !== 'string') bad(`${where}: mime_type required with data`);
      items.push(inlineItem(c.type, c.mime_type, c.data, where));
    } else items.push({ t: 'media', kind: c.type, mime: c.mime_type, source: 'ref', ref: https(c.uri, where) });
  }
  return items;
}
const EXTRACT = { chat: chatItems, responses: responsesItems, messages: messagesItems, gemini: geminiItems, interactions: interactionsItems };

// expected: the caller's semantic view. Wire forms that cannot carry a field (a Files-API id has no
// MIME, a short audio format is not a MIME) leave that field undefined on the wire side.
function matches(wire, want, where) {
  if (wire.t !== want.t) bad(`${where}: part type ${wire.t} != ${want.t}`);
  if (want.t === 'text') { if (wire.text !== want.text) bad(`${where}: text differs`); return 0; }
  if (wire.kind !== undefined && wire.kind !== want.kind) bad(`${where}: kind ${wire.kind} != ${want.kind}`);
  const wantRef = want.source === 'url' || want.source === 'file';
  if (wire.source === 'ref' ? !wantRef : wire.source !== want.source) bad(`${where}: source ${wire.source} != ${want.source}`);
  if (wire.mime !== undefined && wire.mime.toLowerCase() !== want.mime.toLowerCase()) bad(`${where}: mime ${wire.mime} != ${want.mime}`);
  if (wire.format !== undefined && !FORMAT_MIMES[wire.format].includes(want.mime.toLowerCase())) bad(`${where}: audio format ${wire.format} for ${want.mime}`);
  if (want.source === 'inline') { if (wire.sha256 !== want.sha256) bad(`${where}: payload SHA-256 differs`); }
  else if (wire.ref !== want.ref) bad(`${where}: reference differs`);
  if (want.name !== undefined && wire.name !== want.name) bad(`${where}: name`);
  return want.source === 'inline' ? 1 : 0;
}
function subset(actual, wanted, where) {
  if (wanted !== null && typeof wanted === 'object' && !Array.isArray(wanted)) {
    if (actual === null || typeof actual !== 'object') bad(`${where}: object expected`);
    for (const [key, value] of Object.entries(wanted)) subset(actual[key], value, `${where}.${key}`);
  } else if (!isDeepStrictEqual(actual, wanted)) bad(`${where}: ${JSON.stringify(actual)} != ${JSON.stringify(wanted)}`);
}
function examine(req, body, c) {
  const family = c.family;
  const url = req.url;
  const gem = url.startsWith('/v1beta/models/');
  const paths = { chat: ['/v1/chat/completions'], responses: ['/v1/responses'], messages: ['/v1/messages'],
    interactions: ['/v1beta/interactions'],
    gemini: [`/v1beta/models/${c.model}:generateContent`, `/v1beta/models/${c.model}:streamGenerateContent?alt=sse`] };
  if (req.method !== 'POST' || !paths[family].includes(url)) bad(`path ${url}`);
  const key = KEYS[family];
  const h = req.headers;
  if (family === 'messages') { if (h['x-api-key'] !== key || h.authorization || h['anthropic-version'] !== '2023-06-01') bad('auth'); }
  else if (family === 'gemini' || family === 'interactions') { if (h['x-goog-api-key'] !== key || h.authorization || h['x-api-key']) bad('auth'); }
  else if (h.authorization !== `Bearer ${key}` || h['x-api-key'] || h['x-goog-api-key']) bad('auth');
  if (family === 'gemini') {
    if (gem && typeof body.stream !== 'undefined') bad('gemini carries no stream member');
  } else if (typeof body.stream !== 'boolean') bad('stream member');
  const wire = EXTRACT[family](body);
  const want = c.expect;
  if (wire.length !== want.length) bad(`part count ${wire.length} != ${want.length}`);
  let verified = 0;
  wire.forEach((w, i) => { verified += matches(w, want[i], `${family} part ${i}`); });
  if (c.subset) subset(body, c.subset, 'body');
  return verified;
}

// ---------------------------------------------------------------------------------------------
// Response-side builders. A plan is an ordered list of text and media items.
// ---------------------------------------------------------------------------------------------
const usage = {
  chat: { prompt_tokens: 5, completion_tokens: 7, total_tokens: 12 },
  responses: { input_tokens: 5, output_tokens: 7, total_tokens: 12, input_tokens_details: { cached_tokens: 0 }, output_tokens_details: { reasoning_tokens: 0 } },
  messages: { input_tokens: 5, output_tokens: 7 },
  gemini: { promptTokenCount: 5, candidatesTokenCount: 7, totalTokenCount: 12 },
  interactions: { total_input_tokens: 5, total_output_tokens: 7, total_thought_tokens: 0, total_tokens: 12 },
};
const sse = (event, data) => (event ? `event: ${event}\n` : '') + `data: ${typeof data === 'string' ? data : JSON.stringify(data)}\n\n`;
// Payload as it goes on the wire after the armed fault: a base64 string, or the wrong JSON type.
const wirePayload = m => m.b64;
// Each audio delta is canonical base64 of an independent run, including padded intermediate
// runs. Byte concatenation, not base64-string concatenation, is required at the receiver.
function runs(m, sizes) {
  const out = []; let at = 0, i = 0;
  while (at < m.bytes.length) {
    const want = sizes[i++ % sizes.length], piece = m.bytes.subarray(at, at + want);
    out.push(piece.toString('base64')); at += want;
  }
  // Corrupt a late run after real prefix data has arrived. Re-encoding the original fixture
  // here must not accidentally erase an armed corruption of the buffered payload.
  if (m.fault?.payload) {
    let index = out.length - 1;
    if (['non-canonical', 'unpadded'].includes(m.fault.payload)) {
      while (index >= 0 && !out[index].endsWith('=')) --index;
      if (index < 0) throw new Error('payload fault needs a padded audio run');
    }
    out[index] = mutatePayload(out[index], m.fault.payload);
  }
  return out;
}
const RUN_SIZES = [15, 4, 111, 7, 303, 10];
function messageText(plan) { return plan.filter(p => p.t === 'text').map(p => p.text).join(''); }
function pieces(text) { const out = []; for (let i = 0; i < text.length; i += 3) out.push(text.slice(i, i + 3)); return out; }

function chat(model, plan, stream, o) {
  const envelope = { id: 'chatcmpl-media', created: 1, model };
  const text = messageText(plan), audio = plan.find(p => p.t === 'media' && p.kind === 'audio');
  const images = plan.filter(p => p.t === 'media' && p.kind === 'image');
  const url = m => (m.delivery === 'uri' ? m.uri : `data:${m.mime};base64,${wirePayload(m)}`);
  const imageEntries = images.map((m, index) => ({ type: 'image_url', image_url: { url: url(m) }, index, ...(o.extra ? { future_field: { a: 1 } } : {}) }));
  if (!stream) {
    const message = { role: 'assistant', content: text || null };
    if (audio) message.audio = { id: audio.id, expires_at: 1900000000, transcript: audio.transcript, ...(audio.fault?.payload === 'empty' ? {} : { data: wirePayload(audio) }), ...(o.extra ? { future_field: true } : {}) };
    if (images.length) message.images = imageEntries;
    return JSON.stringify({ ...envelope, object: 'chat.completion', choices: [{ index: 0, message, finish_reason: 'stop', logprobs: null }], usage: usage.chat });
  }
  const chunk = (delta, finish = null) => sse('', { ...envelope, object: 'chat.completion.chunk', choices: [{ index: 0, delta, finish_reason: finish, logprobs: null }] });
  let wire = chunk({ role: 'assistant' });
  for (const piece of pieces(text)) wire += chunk({ content: piece });
  if (images.length) wire += chunk({ images: imageEntries }) + (o.repeat_images ? chunk({ images: imageEntries }) : '');
  if (audio) {
    wire += chunk({ audio: { id: audio.id } });
    const parts = audio.fault?.payload === 'empty' ? [] : runs(audio, RUN_SIZES);
    const words = pieces(audio.transcript ?? '');
    parts.forEach((p, i) => {
      wire += chunk({ audio: { data: p, ...(i < words.length ? { transcript: words[i] } : {}) } });
    });
    for (let i = parts.length; i < words.length; ++i) wire += chunk({ audio: { transcript: words[i] } });
    if (!parts.length) for (const w of words) wire += chunk({ audio: { transcript: w } });
  }
  wire += chunk(audio ? { audio: { expires_at: 1900000000 } } : {}, 'stop');
  wire += sse('', { ...envelope, object: 'chat.completion.chunk', choices: [], usage: usage.chat });
  return wire + sse('', '[DONE]');
}

function responses(model, plan, stream, o) {
  const items = plan.map((p, i) => {
    if (p.t === 'text') return { id: `msg_${i}`, type: 'message', status: 'completed', role: 'assistant', content: [{ type: 'output_text', text: p.text, annotations: [] }] };
    const format = { 'image/png': 'png', 'image/jpeg': 'jpeg', 'image/webp': 'webp' }[p.mime] ?? p.output_format;
    return { id: p.id, type: 'image_generation_call', status: 'completed', result: p.fault?.payload === 'empty' ? '' : wirePayload(p),
      action: 'generate', background: 'opaque', output_format: format, quality: 'high', revised_prompt: 'a synthetic image', size: '1024x1024',
      ...(o.extra ? { future_field: [1] } : {}) };
  });
  const response = (output, status, counters) => ({ id: 'resp_media', object: 'response', created_at: 1, model, status, output, usage: counters, incomplete_details: null, error: null });
  if (!stream) return JSON.stringify(response(items, 'completed', usage.responses));
  let sequence = 0;
  const frame = (type, fields) => sse(type, { type, sequence_number: sequence++, ...fields });
  let wire = frame('response.created', { response: response([], 'in_progress', null) });
  items.forEach((item, output_index) => {
    if (item.type === 'message') {
      wire += frame('response.output_item.added', { output_index, item: { ...item, status: 'in_progress', content: [] } });
      const owner = { item_id: item.id, output_index, content_index: 0 };
      const text = item.content[0].text;
      wire += frame('response.content_part.added', { ...owner, part: { ...item.content[0], text: '' } });
      for (const piece of pieces(text)) wire += frame('response.output_text.delta', { ...owner, delta: piece });
      wire += frame('response.output_text.done', { ...owner, text });
      wire += frame('response.content_part.done', { ...owner, part: item.content[0] });
    } else {
      const owner = { item_id: item.id, output_index };
      wire += frame('response.output_item.added', { output_index, item: { id: item.id, type: item.type, status: 'in_progress' } });
      wire += frame('response.image_generation_call.in_progress', owner);
      wire += frame('response.image_generation_call.generating', owner);
      // A preview frame must never be mistaken for the finished image.
      wire += frame('response.image_generation_call.partial_image', { ...owner, partial_image_index: 0, partial_image_b64: Buffer.from('preview-bytes').toString('base64'), output_format: item.output_format });
      wire += frame('response.image_generation_call.completed', owner);
    }
    wire += frame('response.output_item.done', { output_index, item });
  });
  return wire + frame('response.completed', { response: response(items, 'completed', usage.responses) });
}

function messages(model, plan, stream) {
  const text = messageText(plan) || 'ok';
  const message = { id: 'msg_media', type: 'message', role: 'assistant', model, content: [{ type: 'text', text }], stop_reason: 'end_turn', stop_sequence: null, usage: { ...usage.messages } };
  if (!stream) return JSON.stringify(message);
  let wire = sse('message_start', { type: 'message_start', message: { ...message, content: [], stop_reason: null, usage: { input_tokens: 5, output_tokens: 1 } } });
  wire += sse('content_block_start', { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } });
  for (const piece of pieces(text)) wire += sse('content_block_delta', { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: piece } });
  wire += sse('content_block_stop', { type: 'content_block_stop', index: 0 });
  wire += sse('message_delta', { type: 'message_delta', delta: { stop_reason: 'end_turn', stop_sequence: null }, usage: { output_tokens: 7 } });
  return wire + sse('message_stop', { type: 'message_stop' });
}

function geminiPart(p) {
  if (p.t === 'text') return { text: p.text };
  if (p.delivery === 'uri') return { fileData: { mimeType: p.mime, fileUri: p.uri, ...(p.name ? { displayName: p.name } : {}) } };
  const data = p.fault?.payload === 'empty' ? '' : wirePayload(p);
  return { inlineData: { mimeType: p.mime, data, ...(p.name ? { displayName: p.name } : {}) } };
}
function gemini(model, plan, stream, o) {
  const parts = plan.map(geminiPart);
  const body = (parts_, finish, counters) => ({ modelVersion: model, responseId: 'resp-media',
    candidates: [{ index: 0, content: { role: 'model', parts: parts_ }, ...(finish ? { finishReason: finish } : {}) }], ...(counters ? { usageMetadata: counters } : {}) });
  if (!stream) return JSON.stringify(body(parts, 'STOP', usage.gemini));
  let wire = '';
  // Group consecutive parts into one frame when asked to, to cover both packings.
  for (let i = 0; i < parts.length; ++i) {
    const group = o.pack && i + 1 < parts.length ? [parts[i], parts[++i]] : [parts[i]];
    wire += sse('', body(group));
  }
  return wire + sse('', body([], 'STOP', usage.gemini));
}

function interactions(model, plan, stream, o) {
  // Independently authored transcription output: byte indexes (é occupies two bytes),
  // exact duration strings, per-word speaker attribution and ordinary citation metadata.
  const transcriptAnnotations = [
    { type: 'word_info', text: 'Héllo', start_index: 0, end_index: 6, start_offset: '0.100s', end_offset: '0.450s', speaker: 'spk_1' },
    { type: 'word_info', text: 'world', start_index: 7, end_index: 12, start_offset: '0.500s', end_offset: '0.900s', speaker: 'spk_2' },
    { type: 'url_citation', start_index: 0, end_index: 12, url: 'https://example.invalid/transcript-source', title: 'source' },
  ];
  const content = plan.map(p => {
    if (p.t === 'text') return { type: 'text', text: p.text, ...(o.transcription ? { annotations: transcriptAnnotations } : {}) };
    if (p.delivery === 'uri') return { type: p.kind, mime_type: p.mime, uri: p.uri };
    return { type: p.kind, mime_type: p.mime, data: p.fault?.payload === 'empty' ? '' : wirePayload(p), ...(p.kind === 'audio' ? { sample_rate: 24000 } : {}), ...(o.extra ? { future_field: 1 } : {}) };
  });
  const resource = (steps, status = 'completed') => ({ id: 'interaction-media', model, status, ...(steps ? { steps } : {}), usage: usage.interactions });
  if (!stream) return JSON.stringify(resource([{ type: 'model_output', content }]));
  const event = (name, fields) => sse(name, { event_type: name, ...fields });
  let wire = event('interaction.created', { interaction: { id: 'interaction-media', model, status: 'in_progress' } });
  wire += event('step.start', { index: 0, step: { type: 'model_output', content: [] } });
  plan.forEach((p, i) => {
    if (p.t === 'text') {
      for (const piece of pieces(p.text)) wire += event('step.delta', { index: 0, delta: { type: 'text', text: piece } });
      if (o.transcription) for (const annotation of transcriptAnnotations)
        wire += event('step.delta', { index: 0, delta: { type: 'text_annotation_delta', annotations: [annotation] } });
      return;
    }
    if (p.delivery === 'uri' || p.kind !== 'audio') { wire += event('step.delta', { index: 0, delta: content[i] }); return; }
    const parts = p.fault?.payload === 'empty' ? [''] : runs(p, RUN_SIZES);
    parts.forEach(data => { wire += event('step.delta', { index: 0, delta: { type: 'audio', mime_type: p.mime, data, sample_rate: 24000 } }); });
  });
  wire += event('step.stop', { index: 0 });
  if (o.annotation_contradiction) {
    content[0].annotations = content[0].annotations.map((a, i) => i === 0 ? { ...a, speaker: 'wrong-final-speaker' } : a);
  }
  // The documented completed event omits the steps; one scenario repeats them to prove reconciliation.
  wire += event('interaction.completed', { interaction: resource(o.final_steps ? [{ type: 'model_output', content }] : undefined) });
  return wire;
}
const BUILD = { chat, responses, messages, gemini, interactions };

// ---------------------------------------------------------------------------------------------
// Delivery: awkward write boundaries, then optional transport faults.
// ---------------------------------------------------------------------------------------------
const CYCLE = [7, 1, 1, 1, 1, 1, 1, 1, 1, 13, 3, 61, 2, 509, 5, 4099, 11, 1, 1, 2, 16381, 4, 997];
function write(res, piece) { return new Promise((resolve, reject) => res.write(piece, error => (error ? reject(error) : resolve()))); }
// Byte-by-byte for the head and the tail, then a cycle of mismatched sizes (scaled for big bodies).
async function deliver(res, buffer, limit, seed, scale) {
  let at = 0, i = seed;
  const tail = Math.max(0, limit - 90);
  while (at < limit) {
    let n = at < 300 || at >= tail ? 1 : CYCLE[i++ % CYCLE.length] * scale;
    n = Math.min(n, limit - at, at < tail ? tail - at : n);
    await write(res, buffer.subarray(at, at + n));
    at += n;
  }
}
function cutOffset(text, buffer, c) {
  const fault = c.fault;
  const m = c.plan.find(p => p.t === 'media' && p.delivery !== 'uri');
  const payload = (m && m.b64) || '';
  const at = text.indexOf(payload.slice(0, 12));
  if (at < 0) throw new Error('transport fault did not locate actual media payload');
  if (fault.at === 'mid-payload') return Buffer.byteLength(text.slice(0, at + Math.floor(payload.length / 2) + 1));
  if (fault.at === 'after-payload') {
    const end = text.indexOf('\n\n', at + payload.length);
    return Buffer.byteLength(text.slice(0, end < 0 ? at + payload.length : end + 2));
  }
  return Math.floor(buffer.length / 2);
}

const server = http.createServer(async (req, res) => {
  let c;
  // A malformed response can make the client close before our final write. Own response errors
  // separately from the incoming-wire oracle; never let an expected close crash the child.
  res.on('error', error => { if (c) c.response_errors.push(error.code ?? error.name); else ++unexpected; });
  try {
    const chunks = []; let size = 0;
    for await (const chunk of req) { size += chunk.length; if (size > (1 << 24)) throw new Error('request bound'); chunks.push(chunk); }
    const body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    let model = body.model;
    if (req.url.startsWith('/v1beta/models/')) model = /^\/v1beta\/models\/([^:]+):/.exec(req.url)?.[1];
    c = cases.get(model);
    if (!c) { ++unexpected; res.writeHead(400); res.end(); return; }
    ++c.count;
    const stream = c.family === 'gemini' ? req.url.includes(':streamGenerateContent') : body.stream === true;
    try { c.verified += examine(req, body, c); }
    catch (error) { ++c.invalid; c.errors.push(String(error.message).slice(0, 300)); }
    res.socket.setNoDelay(true);
    const text = BUILD[c.family](model, c.plan, stream, c.options);
    const buffer = Buffer.from(text, 'utf8');
    const fault = c.fault && c.fault.transport;
    const limit = fault ? cutOffset(text, buffer, c) : buffer.length;
    if (stream) {
      res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache', Connection: 'close' });
    } else {
      res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': buffer.length, Connection: 'close' });
    }
    res.flushHeaders();
    try { await deliver(res, buffer, limit, c.count * 3, c.scale); }
    catch (error) {
      if (!c.plan.some(p => p.fault) || !['EPIPE', 'ECONNRESET', 'ERR_STREAM_DESTROYED'].includes(error.code)) throw error;
      ++c.aborted;
      res.destroy(); return;
    }
    if (fault === 'reset') { ++c.faults; res.socket.resetAndDestroy(); return; }
    if (fault === 'fin') { ++c.faults; res.socket.end(); return; }
    if (fault === 'clean') { ++c.faults; res.end(); return; }
    res.end();
  } catch (error) {
    if (c) { ++c.invalid; c.errors.push(String(error.message).slice(0, 300)); } else ++unexpected;
    res.destroy();
  }
});
server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
server.on('clientError', (_error, socket) => socket.destroy());

await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
reply({ port: server.address().port });

// ---------------------------------------------------------------------------------------------
// Control channel.
// ---------------------------------------------------------------------------------------------
const snapshot = c => ({ count: c.count, invalid: c.invalid, unexpected, faults: c.faults, verified: c.verified, aborted: c.aborted, response_errors: c.response_errors, errors: c.errors.slice(0, 4) });
function resolvePlan(plan) {
  return plan.map((p, i) => {
    if (p.t === 'text') return p;
    const f = makeFixture(p.fixture, p.size ?? 256, p.salt ?? i, p.mime);
    return { t: 'media', fixture: f, ...f, ...(p.delivery ? { delivery: p.delivery, uri: p.uri } : {}), id: p.id, transcript: p.transcript, name: p.name, output_format: p.output_format, fault: p.fault };
  });
}
function mutatePayload(b, fault) {
  const mid = Math.floor(b.length / 2);
  if (fault === 'wrong-type') return 12345;
  if (fault === 'bad-char') return b.slice(0, mid) + '!' + b.slice(mid + 1);
  if (fault === 'non-canonical') {
    const pad = b.endsWith('==') ? 2 : b.endsWith('=') ? 1 : 0;
    if (!pad) throw new Error('non-canonical needs padded fixture');
    const i = b.length - pad - 1, alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    return b.slice(0, i) + alphabet[(alphabet.indexOf(b[i]) + 1) % 64] + b.slice(i + 1);
  }
  if (fault === 'unpadded') return b.replace(/=+$/, '');
  if (fault === 'truncated-quartet') return b.slice(0, b.length - 3);
  return b;
}
function mutate(plan) {
  for (const m of plan) if (m.t === 'media' && m.fault) m.b64 = mutatePayload(m.b64, m.fault.payload);
}
const control = readline.createInterface({ input: process.stdin });
control.on('line', line => {
  try {
    const command = JSON.parse(line);
    if (command.fixture) {
      const f = makeFixture(command.fixture, command.size ?? 256, command.salt ?? 0, command.mime);
      reply({ mime: f.mime, kind: f.kind, size: f.bytes.length, sha256: f.sha256, ...(f.bytes.length <= 262144 ? { base64: f.b64 } : {}) });
    } else if (command.arm) {
      if (cases.has(command.arm)) throw new Error('duplicate');
      const plan = resolvePlan(command.plan ?? [{ t: 'text', text: 'ok' }]);
      mutate(plan);
      const c = { model: command.arm, family: command.family, expect: command.expect ?? [], plan, subset: command.subset, fault: command.fault,
        options: command.options ?? {}, scale: command.scale ?? 1, count: 0, invalid: 0, faults: 0, verified: 0, aborted: 0, response_errors: [], errors: [] };
      cases.set(command.arm, c);
      reply({ armed: true, outputs: plan.filter(p => p.t === 'media').map(p => ({ sha256: p.sha256, size: p.bytes.length, mime: p.mime, kind: p.kind })) });
    } else if (command.model) {
      const c = cases.get(command.model);
      if (!c) throw new Error('unknown');
      reply(snapshot(c));
    } else throw new Error('command');
  } catch (error) { reply({ error: String(error.message) }); }
});
control.on('close', () => { server.close(); for (const socket of sockets) socket.destroy(); });

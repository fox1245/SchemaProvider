// Synthetic provider model listings for the catalogue refresh test. No provider API is called.
// Each source is served under its own prefix: /<scenario>/<listing path>. A request without the
// expected credential header gets 401, so a refresh that succeeds proves the headers were sent.
import http from 'node:http';

const keys = { gemini: 'gemini-test-key', anthropic: 'anthropic-test-key', openai: 'openai-test-key' };
const openrouter = (version) => ({
  data: [
    {
      id: 'vendor/text-model', canonical_slug: 'vendor/text-model-20261001', name: 'Vendor: Text Model',
      context_length: 128000, top_provider: { context_length: 128000, max_completion_tokens: version === 1 ? 8192 : 16384 },
      architecture: { input_modalities: ['text', 'image'], output_modalities: ['text'] },
      pricing: { prompt: '0.0000001', completion: '0.0000004', overrides: [{ prompt: '0.0000002' }] },
      supported_parameters: ['temperature', 'tools', 'bad token!'],
      benchmarks: { nested: { deeper: [1, 2, 3] } },
    },
    { id: 'vendor/image-model', architecture: { input_modalities: ['text'], output_modalities: ['image'] }, pricing: { image: '0.04', web_search: '1e-2' } },
    ...(version === 1 ? [{ id: 'vendor/retiring', expiration_date: '2026-11-01', architecture: { output_modalities: ['text'] } }] : []),
    { id: '~vendor/text-latest', alias_target: { name: 'Vendor: Text Model', slug: 'vendor/text-model' }, architecture: { output_modalities: ['text'] } },
    { id: 'has space' },
  ],
});
const gemini = {
  first: { models: [
    { name: 'models/gemini-test-flash', displayName: 'Gemini Test Flash', inputTokenLimit: 1048576, outputTokenLimit: 65536,
      supportedGenerationMethods: ['generateContent', 'countTokens'], thinking: true },
  ], nextPageToken: 'page/2+' },
  second: { models: [
    { name: 'models/veo-test', supportedGenerationMethods: ['predictLongRunning'] },
    { name: 'tunedModels/not-a-model' },
  ] },
};
const anthropic = {
  first: { data: [{ type: 'model', id: 'claude-test-a', display_name: 'Claude Test A', max_input_tokens: 200000, max_tokens: 64000,
    capabilities: { image_input: { supported: true }, pdf_input: { supported: false } }, lifecycle: 'active', retires_at: null }],
    has_more: true, last_id: 'claude-test-a' },
  second: { data: [{ type: 'model', id: 'claude-test-b', lifecycle: 'deprecated', retires_at: '2026-12-01T00:00:00Z' }], has_more: false },
};
const openai = { object: 'list', data: [{ id: 'gpt-test-1', object: 'model', owned_by: 'openai' }, { id: 'tts-test', shutdown_date: '2027-01-06' }] };
// A listing that names a model the embedded catalogue has verified facts for.
const openaiBuiltin = { object: 'list', data: [{ id: 'gpt-6-luna', object: 'model' }, { id: 'gpt-test-1', object: 'model' }] };

const send = (res, status, body) => {
  res.writeHead(status, { 'content-type': 'application/json' });
  res.end(typeof body === 'string' ? body : JSON.stringify(body));
};
// The requests each scenario received, which the test reads over the control channel: all of them, and the
// model ids whose endpoints listing was asked for, in order.
const seen = {};
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  const [, scenario, ...rest] = url.pathname.split('/');
  const path = '/' + rest.join('/');
  const h = req.headers;
  const counts = (seen[scenario] ??= { total: 0, endpoints: [] });
  counts.total += 1;
  if (path.startsWith(endpointsPrefix) && path.endsWith(endpointsSuffix)) counts.endpoints.push(path.slice(endpointsPrefix.length, -endpointsSuffix.length));
  if (ceilings(scenario, path, url, h, res)) return;
  if (listingExtras(scenario, path, h, res)) return;
  if (scenario === 'broken') return send(res, 500, { error: { message: 'synthetic outage' } });
  if (scenario === 'notjson') return send(res, 200, 'not a listing');
  if (scenario.startsWith('openrouter')) {
    if (path !== '/api/v1/models' || url.searchParams.get('output_modalities') !== 'all') return send(res, 404, {});
    if (h.authorization && h.authorization !== 'Bearer openrouter-test-key') return send(res, 401, {});
    return send(res, 200, openrouter(scenario === 'openrouter-v2' ? 2 : 1));
  }
  if (scenario === 'gemini' || scenario === 'gemini-loop') {
    if (path !== '/v1beta/models' || url.searchParams.get('pageSize') !== '1000') return send(res, 404, {});
    if (h['x-goog-api-key'] !== keys.gemini) return send(res, 401, {});
    if (scenario === 'gemini-loop') return send(res, 200, { models: [], nextPageToken: 'same' });
    const token = url.searchParams.get('pageToken');
    if (token === null) return send(res, 200, gemini.first);
    return token === 'page/2+' ? send(res, 200, gemini.second) : send(res, 400, {});
  }
  if (scenario === 'anthropic') {
    if (path !== '/v1/models' || url.searchParams.get('limit') !== '1000') return send(res, 404, {});
    if (h['x-api-key'] !== keys.anthropic || h['anthropic-version'] !== '2023-06-01') return send(res, 401, {});
    const after = url.searchParams.get('after_id');
    if (after === null) return send(res, 200, anthropic.first);
    return after === 'claude-test-a' ? send(res, 200, anthropic.second) : send(res, 400, {});
  }
  if (scenario === 'openai' || scenario === 'openai-builtin') {
    if (path !== '/v1/models') return send(res, 404, {});
    if (h.authorization !== `Bearer ${keys.openai}`) return send(res, 401, {});
    return send(res, 200, scenario === 'openai' ? openai : openaiBuiltin);
  }
  send(res, 404, {});
});
server.listen(0, '127.0.0.1', () => process.stdout.write(JSON.stringify({ port: server.address().port }) + '\n'));
// Control lines on stdin, each answered with one JSON line on stdout:
//   {"requests": scenario}             the requests that scenario received
//   {"set": scenario, "models": {...}}  the endpoints of each model of a ceilings scenario: id -> [pricing, ...]
let control = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => {
  control += chunk;
  for (let cut = control.indexOf('\n'); cut >= 0; cut = control.indexOf('\n')) {
    const line = control.slice(0, cut);
    control = control.slice(cut + 1);
    let answer;
    try {
      const command = JSON.parse(line);
      if (typeof command.requests === 'string') {
        const counts = seen[command.requests] ?? { total: 0, endpoints: [] };
        answer = { total: counts.total, endpoints: counts.endpoints };
      } else if (typeof command.set === 'string' && command.models && typeof command.models === 'object') {
        generated[command.set] = Object.fromEntries(Object.entries(command.models).map(([id, prices]) => [id, { endpoints: prices.map(endpoint) }]));
        answer = { set: command.set };
      } else answer = { error: 'unknown command' };
    } catch {
      answer = { error: 'malformed command' };
    }
    process.stdout.write(JSON.stringify(answer) + '\n');
  }
});
process.stdin.on('end', () => server.close(() => process.exit(0)));
process.stdin.resume();

// OpenRouter models whose route ceilings a refresh reads from their endpoints listings
// (GET /api/v1/models/{author}/{slug}/endpoints). Scenario `ceilings` is the first state, `ceilings-v2` changes
// one model's prices, makes two listings fail and one malformed, and a scenario set over the control channel
// serves generated endpoints. Every listing also names `vendor/unasked`, whose endpoints no test asks for.
const endpointsPrefix = '/api/v1/models/';
const endpointsSuffix = '/endpoints';
const generated = {};
const endpoint = (pricing) => ({
  name: 'Provider | model', model_id: 'vendor/model-20261001', model_name: 'Model', context_length: 131072, pricing,
  provider_name: 'Provider', tag: 'provider', service_tier: null, quantization: 'fp8', max_completion_tokens: 16384,
  max_prompt_tokens: null, supported_parameters: ['tools'], supports_tool_choice: {}, uptime_last_30m: 99.5, status: 0,
});
const price = (prompt, completion, more = {}) => ({ prompt, completion, ...more });
const ceilingDocs = (version) => ({
  // The dearest of several endpoints, unit by unit; the third endpoint writes a trailing zero.
  'vendor/many': { endpoints: [price('0.000001', '0.000004'), price('0.000003', '0.000002'), price('0.0000020', version === 1 ? '0.000005' : '0.000006')] },
  // Request fees at every endpoint, one of them zero; zero fees only; and a fee only some endpoints list.
  'vendor/fee': version === 1
    ? { endpoints: [price('0.000001', '0.000001', { request: '0' }), price('0.000001', '0.000001', { request: '0.0002' }), price('0.000001', '0.000001', { request: '0.0001' })] }
    : { body: '<html>captive portal</html>' },
  'vendor/zero-fee': { endpoints: [price('0.000001', '0.000002', { request: '0' }), price('0.000001', '0.000002', { request: '0' })] },
  'vendor/missing-fee': { endpoints: [price('0.000001', '0.000002', { request: '0.001' }), price('0.000001', '0.000002')] },
  // Cache, reasoning and long-context prices above the prompt and completion prices, a discount, and prices per
  // image and per search, which no token ceiling covers.
  'vendor/cached': { endpoints: [
    price('0.000003', '0.000015', { input_cache_read: '0.0000003', input_cache_write: '0.00000375', input_cache_write_1h: '0.000006',
      internal_reasoning: '0.000015', image: '0.04', web_search: '0.01', discount: 0.25 }),
    price('0.000002', '0.00001', { internal_reasoning: '0.00002', discount: 0,
      overrides: [{ min_prompt_tokens: 200000, prompt: '0.000004', completion: '0.000018' }, { utc_start: 100, utc_end: 400, prompt: '0.000001' }] }),
  ] },
  '~vendor/alias-latest': { endpoints: [] },
  // One endpoint among usable ones with a price that cannot be read.
  'vendor/missing-prompt': { endpoints: [price('0.000001', '0.000002'), { completion: '0.000003' }] },
  'vendor/negative': { endpoints: [price('0.000001', '0.000002'), price('-1', '0.000001')] },
  'vendor/not-decimal': { endpoints: [price('0.000001', 'free'), price('0.000002', '0.000001')] },
  'vendor/number': { endpoints: [price(0.000001, '0.000002'), price('0.000002', '0.000001')] },
  'vendor/too-large': { endpoints: [price('99999', '0.000001'), price('0.000001', '0.000001')] },
  'vendor/bad-cache': { endpoints: [price('0.000001', '0.000002', { input_cache_write: '-1' }), price('0.000001', '0.000002')] },
  'vendor/bad-override': { endpoints: [price('0.000001', '0.000002', { overrides: [{ min_prompt_tokens: 128000, completion: '1e-5' }] })] },
  'vendor/bad-fee': { endpoints: [price('0.000001', '0.000002', { request: '-1' }), price('0.000001', '0.000002', { request: '0.001' })] },
  'vendor/bad-discount': { endpoints: [price('0.000001', '0.000002', { discount: -0.5 })] },
  // Documents that are not the documented shape.
  'vendor/malformed': { data: { id: 'vendor/malformed', name: 'Malformed', endpoints: {} } },
  'vendor/override-shape': { endpoints: [price('0.000001', '0.000002', { overrides: ['long context'] })] },
  'vendor/wrong-id': { id: 'vendor/other', endpoints: [price('0.000001', '0.000002')] },
  'vendor/not-json': { body: '<html>captive portal</html>' },
  // Listings that fail in version 2: an outage and one slower than the test's page timeout.
  'vendor/outage': version === 1 ? { endpoints: [price('0.000001', '0.000002', { request: '0.0001' })] } : { status: 500 },
  'vendor/stall': version === 1 ? { endpoints: [price('0.000002', '0.000003')] } : { delay: 3000, endpoints: [price('0.000009', '0.000009')] },
  // The listing gives this model the 64 price units an entry holds, so no ceiling fits beside them.
  'vendor/full': { endpoints: [price('0.000001', '0.000002')] },
});
const listedEntry = (id) => ({
  id, name: `Vendor: ${id}`, context_length: 131072, architecture: { input_modalities: ['text'], output_modalities: ['text'] },
  pricing: id === 'vendor/full' ? Object.fromEntries(Array.from({ length: 64 }, (_, i) => [`unit_${i}`, '0'])) : { prompt: '0.000001', completion: '0.000002' },
});
function ceilings(scenario, path, url, h, res) {
  if (!scenario.startsWith('ceilings')) return false;
  if (h.authorization && h.authorization !== 'Bearer openrouter-test-key') return send(res, 401, {}), true;
  const docs = generated[scenario] ?? ceilingDocs(scenario === 'ceilings-v2' ? 2 : 1);
  if (path === '/api/v1/models') {
    if (url.searchParams.get('output_modalities') !== 'all') return send(res, 404, {}), true;
    return send(res, 200, { data: [...Object.keys(docs), 'vendor/unasked'].map(listedEntry) }), true;
  }
  if (!path.startsWith(endpointsPrefix) || !path.endsWith(endpointsSuffix) || url.search !== '') return send(res, 404, {}), true;
  const id = path.slice(endpointsPrefix.length, -endpointsSuffix.length);
  const doc = id === 'vendor/unasked' ? { endpoints: [endpoint(price('1', '1'))] } : docs[id];
  if (!doc) return send(res, 404, { error: { code: 404, message: 'synthetic: no such model' } }), true;
  if (doc.status) return send(res, doc.status, { error: { code: doc.status, message: 'synthetic outage' } }), true;
  if (doc.body) return send(res, 200, doc.body), true;
  const body = doc.data ? { data: doc.data } : {
    data: { id: doc.id ?? id, name: `Vendor: ${id}`, created: 1790000000, description: 'Synthetic model',
      architecture: { input_modalities: ['text'], output_modalities: ['text'], modality: 'text->text', tokenizer: 'Other', instruct_type: null },
      endpoints: doc.endpoints.map((e) => (e.name ? e : endpoint(e))) },
  };
  if (doc.delay) setTimeout(() => send(res, 200, body), doc.delay);
  else send(res, 200, body);
  return true;
}

// Listings a refresh has to fail on, or to take care with: empty, shrunk, with a page token that cannot
// continue them, too large for a catalogue, listed by two sources, or slow. Each checks its credential.
const ids = (prefix, count) => Array.from({ length: count }, (_, i) => ({ id: `${prefix}${i}` }));
const openaiList = (data) => ({ object: 'list', data });
const geminiList = (names, next) => ({
  models: names.map((name) => ({ name: `models/${name}`, supportedGenerationMethods: ['generateContent'] })),
  ...(next === undefined ? {} : { nextPageToken: next }),
});
const extras = {
  'openai-empty': { kind: 'openai', body: () => openaiList([]) },
  'openai-large': { kind: 'openai', body: () => openaiList(ids('large-', 6)) },
  'openai-half': { kind: 'openai', body: () => openaiList(ids('large-', 3)) },
  'openai-small': { kind: 'openai', body: () => openaiList(ids('large-', 2)) },
  'openai-huge': { kind: 'openai', body: () => openaiList(ids('huge-', 16385)) },
  'openai-wide-a': { kind: 'openai', body: () => openaiList(ids('a-', 10000)) },
  'openai-wide-b': { kind: 'openai', body: () => openaiList(ids('b-', 10000)) },
  'openai-stall': { kind: 'openai', delay: 3000, body: () => openaiList(ids('late-', 1)) },
  'openai-shared': { kind: 'openai', body: () => openaiList([{ id: 'shared-model' }, { id: 'solo-openai' }]) },
  'openai-shared-2': { kind: 'openai', body: () => openaiList([{ id: 'solo-openai' }]) },
  'gemini-empty': { kind: 'gemini', body: () => ({}) },
  'gemini-token-empty': { kind: 'gemini', body: () => geminiList(['gemini-partial'], '') },
  'gemini-token-space': { kind: 'gemini', body: () => geminiList(['gemini-partial'], 'two words') },
  'gemini-token-long': { kind: 'gemini', body: () => geminiList(['gemini-partial'], 'x'.repeat(2000)) },
  'gemini-shared': { kind: 'gemini', body: () => geminiList(['shared-model']) },
  'gemini-shared-2': { kind: 'gemini', body: () => geminiList(['gemini-other']) },
  'anthropic-empty': { kind: 'anthropic', body: () => ({ data: [], has_more: false }) },
};
function listingExtras(scenario, path, h, res) {
  const extra = extras[scenario];
  if (!extra) return false;
  const authorized = {
    openai: path === '/v1/models' && h.authorization === `Bearer ${keys.openai}`,
    gemini: path === '/v1beta/models' && h['x-goog-api-key'] === keys.gemini,
    anthropic: path === '/v1/models' && h['x-api-key'] === keys.anthropic && h['anthropic-version'] === '2023-06-01',
  }[extra.kind];
  if (!authorized) send(res, 401, {});
  else if (extra.delay) setTimeout(() => send(res, 200, extra.body()), extra.delay);
  else send(res, 200, extra.body());
  return true;
}

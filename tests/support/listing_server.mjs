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
    { id: 'vendor/image-model', architecture: { input_modalities: ['text'], output_modalities: ['image'] }, pricing: { image: '0.04' } },
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

const send = (res, status, body) => {
  res.writeHead(status, { 'content-type': 'application/json' });
  res.end(typeof body === 'string' ? body : JSON.stringify(body));
};
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  const [, scenario, ...rest] = url.pathname.split('/');
  const path = '/' + rest.join('/');
  const h = req.headers;
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
  if (scenario === 'openai') {
    if (path !== '/v1/models') return send(res, 404, {});
    if (h.authorization !== `Bearer ${keys.openai}`) return send(res, 401, {});
    return send(res, 200, openai);
  }
  send(res, 404, {});
});
server.listen(0, '127.0.0.1', () => process.stdout.write(JSON.stringify({ port: server.address().port }) + '\n'));
process.stdin.on('end', () => server.close(() => process.exit(0)));
process.stdin.resume();

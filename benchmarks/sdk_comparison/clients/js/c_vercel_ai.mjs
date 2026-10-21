// Vercel AI SDK (`ai` with `@ai-sdk/openai` and `@ai-sdk/anthropic`): generateText and streamText under Node.
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { generateText, streamText } from 'ai';
import { createOpenAI } from '@ai-sdk/openai';
import { createAnthropic } from '@ai-sdk/anthropic';
import { main } from './harness.mjs';

const require = createRequire(import.meta.url);
const versionOf = (name) => JSON.parse(readFileSync(require.resolve(`${name}/package.json`), 'utf8')).version;

function setup(a) {
  const model = a.api === 'openai'
    // `.chat` is the Chat Completions API; `openai(model)` would use the Responses API.
    ? createOpenAI({ baseURL: `${a.basePrefix}/v1`, apiKey: a.apiKey }).chat(a.model)
    : createAnthropic({ baseURL: `${a.basePrefix}/v1`, apiKey: a.apiKey })(a.model);
  return { model };
}

async function call(ctx, a) {
  const options = { model: ctx.model, system: a.system, prompt: a.user, maxOutputTokens: a.maxTokens };
  if (a.mode === 'buffered') return generateText(options);
  const parts = [];
  const result = streamText({ ...options, onError: ({ error }) => { throw error; } });
  for await (const delta of result.textStream) if (delta) parts.push(delta);
  return parts;
}

const extract = (result) => result.text;

const describe = (a) => ({
  concurrency_model: 'one event loop; N async workers awaiting generateText / streamText (Promise.all)',
  http: '1.1 (Node fetch, HTTP/2 not enabled)',
  pool: "Node's global fetch dispatcher (undici Agent) with no connection limit, so the pool follows the concurrency",
  ca: 'NODE_EXTRA_CA_CERTS', retries: 'SDK default (maxRetries=2), not triggered', telemetry: 'none unless experimental_telemetry is set',
  node: process.version, ai: versionOf('ai'), openai: versionOf('@ai-sdk/openai'), anthropic: versionOf('@ai-sdk/anthropic'),
});

main({ client: 'vercel-ai', version: versionOf('ai'), setup, call, extract, describe });

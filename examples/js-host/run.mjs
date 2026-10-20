// Runs the example under Node 18+: node run.mjs <sp_js_host_example.js> <model> <prompt> [stream] [base url]
// The key is read from the environment (OPENROUTER_API_KEY) and never printed.
import { createRequire } from 'node:module';
import path from 'node:path';

const [, , modulePath, model, prompt, stream, base = ''] = process.argv;
const createModule = createRequire(import.meta.url)(path.resolve(modulePath));
const outcome = new Promise((resolve) => {
  createModule({ onOutcome: (json) => resolve(JSON.parse(json)) }).then((mod) => {
    mod.ccall('sp_example_chat', null, ['string', 'string', 'string', 'string', 'number'],
              [base, process.env.OPENROUTER_API_KEY ?? '', model, prompt, stream === 'stream' ? 1 : 0]);
  });
});
console.log(JSON.stringify(await outcome));
process.exit(0);

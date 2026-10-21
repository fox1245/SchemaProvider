"""LiteLLM Python SDK: litellm.acompletion (asyncio)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402


def _args_concurrency():
    # The pool limits are read from the environment when litellm is imported, so size them before the import.
    for i, token in enumerate(sys.argv):
        if token == "--concurrency":
            return sys.argv[i + 1]
    return "1"


os.environ["AIOHTTP_CONNECTOR_LIMIT"] = _args_concurrency()
os.environ["AIOHTTP_CONNECTOR_LIMIT_PER_HOST"] = _args_concurrency()

import litellm  # noqa: E402

litellm.telemetry = False
litellm.suppress_debug_info = True
litellm.set_verbose = False


def setup(a):
    return {"model": ("openai/" if a.api == "openai" else "anthropic/") + a.model,
            "api_base": a.base_prefix + ("/v1" if a.api == "openai" else "")}


async def call(ctx, a):
    messages = [{"role": "system", "content": a.system}, {"role": "user", "content": a.user}]
    kw = dict(model=ctx["model"], messages=messages, api_base=ctx["api_base"], api_key=a.api_key, max_tokens=a.max_tokens)
    if a.mode == "buffered":
        return await litellm.acompletion(**kw)
    parts = []
    stream = await litellm.acompletion(stream=True, **kw)
    async for chunk in stream:
        if chunk.choices:
            text = chunk.choices[0].delta.content
            if text:
                parts.append(text)
    return parts


def extract(response):
    return response.choices[0].message.content


def describe(a):
    return {
        "concurrency_model": "asyncio, litellm.acompletion from N tasks; litellm keeps its own cached client sessions",
        "http": "1.1", "pool": f"AIOHTTP_CONNECTOR_LIMIT=AIOHTTP_CONNECTOR_LIMIT_PER_HOST={a.concurrency} (environment, read at import)",
        "ca": "SSL_CERT_FILE (litellm.ssl_verify=True)", "retries": "SDK default, not triggered",
        "telemetry": "litellm.telemetry=False; no callbacks registered", "model_string": ctx_model(a),
    }


def ctx_model(a):
    return ("openai/" if a.api == "openai" else "anthropic/") + a.model


if __name__ == "__main__":
    harness.main("litellm", harness.dist_version("litellm"), setup, call, extract, describe)

"""aisuite: aisuite.Client().chat.completions.acreate (asyncio) over the OpenAI and Anthropic providers."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

import aisuite  # noqa: E402


def setup(a):
    import anthropic
    import openai

    sdk = openai if a.api == "openai" else anthropic
    a.underlying = f"{sdk.__name__} {sdk.__version__}"
    # The provider configuration is passed to the sync and to the async client constructor alike, so an http_client
    # (which is either one or the other) cannot be given; the underlying SDK's default pool is used.
    config = {"api_key": a.api_key, "base_url": a.base_prefix + ("/v1" if a.api == "openai" else "")}
    a.provider_key = "openai" if a.api == "openai" else "anthropic"
    return aisuite.Client(provider_configs={a.provider_key: config})


async def call(client, a):
    # A fresh list per call: aisuite's Anthropic provider removes the system message from the list it is given.
    messages = [{"role": "system", "content": a.system}, {"role": "user", "content": a.user}]
    kw = dict(model=f"{a.provider_key}:{a.model}", messages=messages, max_tokens=a.max_tokens)
    if a.mode == "buffered":
        return await client.chat.completions.acreate(**kw)
    parts = []
    stream = await client.chat.completions.acreate(stream=True, **kw)
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
        "concurrency_model": "asyncio, Client.chat.completions.acreate from N tasks"
        + ("; this call has no native async path in aisuite's Anthropic provider and runs the synchronous SDK call in asyncio's default thread pool"
           if a.api == "anthropic" and a.mode == "buffered" else ""), "http": "1.1 (HTTP/2 not enabled)",
        "underlying_sdk": a.underlying,
        "pool": "underlying SDK default (max_connections=1000, max_keepalive_connections=100); aisuite's provider configuration "
                "builds a sync and an async client from the same arguments, so a pool cannot be sized through it",
        "ca": "SSL_CERT_FILE through the HTTP library's default trust", "retries": "underlying SDK default, not triggered",
        "telemetry": "no trace sink registered",
    }


if __name__ == "__main__":
    harness.main("aisuite", harness.dist_version("aisuite"), setup, call, extract, describe)

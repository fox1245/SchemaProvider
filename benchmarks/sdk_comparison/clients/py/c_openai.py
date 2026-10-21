"""Official `openai` Python SDK: the single-provider baseline for OpenAI Chat Completions (AsyncOpenAI, asyncio)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

import openai  # noqa: E402


def setup(a):
    if a.api != "openai":
        raise SystemExit("openai supports the Chat Completions cells only")
    limits, http_lib = harness.sdk_limits(openai.DefaultAsyncHttpxClient, a.concurrency)
    a.http_lib = http_lib
    # The SDK's default client with its connection limits sized to the concurrency; everything else is the default.
    http = openai.DefaultAsyncHttpxClient(limits=limits)
    return openai.AsyncOpenAI(api_key=a.api_key, base_url=a.base_prefix + "/v1", http_client=http)


def messages(a):
    return [{"role": "system", "content": a.system}, {"role": "user", "content": a.user}]


async def call(client, a):
    if a.mode == "buffered":
        return await client.chat.completions.create(model=a.model, messages=messages(a), max_tokens=a.max_tokens)
    parts = []
    stream = await client.chat.completions.create(model=a.model, messages=messages(a), max_tokens=a.max_tokens, stream=True)
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
        "concurrency_model": "asyncio, one AsyncOpenAI client shared by all tasks", "http": "1.1 (HTTP/2 not enabled)",
        "http_library": a.http_lib, "pool": f"max_connections=max_keepalive_connections={a.concurrency}",
        "ca": "SSL_CERT_FILE through the HTTP library's default trust", "retries": "SDK default (max_retries=2), not triggered",
        "telemetry": "none in the SDK",
    }


if __name__ == "__main__":
    harness.main("openai-py", openai.__version__, setup, call, extract, describe)

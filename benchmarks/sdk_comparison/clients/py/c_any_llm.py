"""Mozilla.ai any-llm (PyPI: any-llm-sdk): AnyLLM.create(...).acompletion, the class README recommends for production."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness  # noqa: E402

from any_llm import AnyLLM  # noqa: E402


def setup(a):
    import anthropic
    import openai

    sdk = openai if a.api == "openai" else anthropic
    limits, http_lib = harness.sdk_limits(sdk.DefaultAsyncHttpxClient, a.concurrency)
    a.http_lib = http_lib
    http = sdk.DefaultAsyncHttpxClient(limits=limits)
    provider = "openai" if a.api == "openai" else "anthropic"
    a.underlying = f"{sdk.__name__} {sdk.__version__}"
    return AnyLLM.create(provider, api_key=a.api_key, api_base=a.base_prefix + ("/v1" if a.api == "openai" else ""), http_client=http)


async def call(llm, a):
    messages = [{"role": "system", "content": a.system}, {"role": "user", "content": a.user}]
    kw = dict(model=a.model, messages=messages, max_tokens=a.max_tokens)
    if a.mode == "buffered":
        return await llm.acompletion(**kw)
    parts = []
    stream = await llm.acompletion(stream=True, **kw)
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
        "concurrency_model": "asyncio, one AnyLLM provider instance shared by all tasks", "http": "1.1 (HTTP/2 not enabled)",
        "http_library": a.http_lib, "underlying_sdk": a.underlying,
        "pool": f"http_client with max_connections=max_keepalive_connections={a.concurrency}, passed through AnyLLM.create",
        "ca": "SSL_CERT_FILE through the HTTP library's default trust", "retries": "underlying SDK default, not triggered",
        "telemetry": "none enabled (the platform extra is not installed)",
    }


if __name__ == "__main__":
    harness.main("any-llm", harness.dist_version("any-llm-sdk"), setup, call, extract, describe)

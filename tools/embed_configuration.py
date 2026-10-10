#!/usr/bin/env python3
"""Emit deterministic typed operational defaults from closed JSON inputs."""
import argparse
import json
import math
from pathlib import Path

FIELDS = (
    'workers io_threads resolver_threads max_host_connections max_head_bytes dns_ttl_seconds '
    'max_operations queued_body_chunks queued_body_bytes max_response_bytes max_error_bytes '
    'semantic_max_parts semantic_max_content_bytes semantic_max_tool_bytes semantic_max_json_depth '
    'sse_max_line_bytes sse_max_event_bytes sse_max_total_bytes default_timeout_ms '
    'slow_callback_threshold_ms retry_tokens retry_tokens_per_second retry_enabled '
    'retry_allow_duplicate_billing_risk retry_max_attempts retry_base_delay_ms retry_max_delay_ms'
).split()
BOOLS = {'retry_enabled', 'retry_allow_duplicate_billing_risk'}
EXTRA = {'total_threads', 'api_key_bytes', 'configuration_bytes', 'configuration_depth', 'error_json_depth'}
KINDS = {'InvalidRequest', 'Authentication', 'Permission', 'NotFound', 'RateLimited',
         'QuotaExhausted', 'LimitUnknown', 'Overloaded', 'RemoteFailure'}
RETRIES = {'Never', 'Transient', 'AfterReset', 'Unknown'}
RESOURCES = ('json_bytes json_depth request_bytes chat_text_request_bytes request_messages request_tools '
             'request_parts native_bytes native_depth native_members image_decoded_bytes audio_decoded_bytes '
             'video_decoded_bytes document_decoded_bytes descriptor_bytes '
             'descriptor_depth policy_bytes policy_depth').split()

def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate configuration key')
        result[key] = value
    return result

def closed(value, fields):
    if not isinstance(value, dict) or set(value) != set(fields):
        raise ValueError('configuration object fields rejected')

def number(value, key):
    if key == 'retry_tokens_per_second':
        if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
            raise ValueError('configuration rate rejected')
    elif type(value) is not int or not 0 <= value <= 9223372036854775807:
        raise ValueError('configuration integer rejected')

def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite configuration')))

def validate(runtime, errors):
    closed(runtime, {'version', 'defaults', 'admission'})
    closed(runtime['defaults'], FIELDS)
    closed(runtime['admission'], (set(FIELDS) - BOOLS) | EXTRA)
    if type(runtime['version']) is not int or runtime['version'] != 1:
        raise ValueError('configuration version rejected')
    for key, value in runtime['defaults'].items():
        if key in BOOLS:
            if type(value) is not bool:
                raise ValueError('configuration boolean rejected')
        else:
            number(value, key)
            if value > runtime['admission'][key]:
                raise ValueError('configuration admission exceeded')
    for key, value in runtime['admission'].items():
        number(value, key)
    zero_allowed = {'max_host_connections', 'dns_ttl_seconds', 'slow_callback_threshold_ms',
                    'retry_tokens', 'retry_tokens_per_second', 'retry_base_delay_ms', 'retry_max_delay_ms'}
    if any(value <= 0 for key, value in runtime['defaults'].items() if key not in BOOLS | zero_allowed):
        raise ValueError('configuration positive value required')
    if any(value <= 0 for key, value in runtime['admission'].items() if key not in zero_allowed):
        raise ValueError('configuration ceiling rejected')
    if sum(runtime['defaults'][k] for k in ('workers', 'io_threads', 'resolver_threads')) > runtime['admission']['total_threads']:
        raise ValueError('configuration thread admission exceeded')
    if runtime['defaults']['retry_base_delay_ms'] > runtime['defaults']['retry_max_delay_ms']:
        raise ValueError('configuration retry delay rejected')
    if runtime['admission']['retry_max_attempts'] > 4294967295 or any(runtime['admission'][k] > 4294967295 for k in ('io_threads', 'resolver_threads')):
        raise ValueError('configuration integer representability rejected')
    closed(errors, {'version', 'statuses', 'families', 'headers'})
    if type(errors['version']) is not int or errors['version'] != 1:
        raise ValueError('error policy version rejected')
    for key in ('statuses', 'families', 'headers'):
        if not isinstance(errors[key], list):
            raise ValueError('error policy array required')
    def classification(rule):
        if rule['kind'] not in KINDS or rule['retry'] not in RETRIES:
            raise ValueError('error classification rejected')
    seen = set()
    for rule in errors['statuses']:
        closed(rule, {'status', 'kind', 'retry'})
        if type(rule['status']) is not int or not 300 <= rule['status'] <= 599 or rule['status'] in seen:
            raise ValueError('error status rejected')
        seen.add(rule['status'])
        classification(rule)
    seen = set()
    for family in errors['families']:
        closed(family, {'family', 'root_error_type', 'error_paths', 'code_fields', 'codes'})
        if not isinstance(family['family'], str) or not family['family'] or family['family'] in seen:
            raise ValueError('error family rejected')
        seen.add(family['family'])
        if not isinstance(family['root_error_type'], str) or not family['root_error_type']:
            raise ValueError('root error discriminator rejected')
        for key, allowed in [('error_paths', {'error', 'response.error', 'root_error'}), ('code_fields', {'type', 'code', 'details.reason'})]:
            values = family[key]
            if not isinstance(values, list) or not values or any(v not in allowed for v in values) or len(set(values)) != len(values):
                raise ValueError('error extraction rejected')
        if not isinstance(family['codes'], list):
            raise ValueError('error codes array required')
        codes = set()
        for rule in family['codes']:
            closed(rule, {'value', 'kind', 'retry'})
            if not isinstance(rule['value'], str) or not rule['value'] or rule['value'] in codes:
                raise ValueError('error code rejected')
            codes.add(rule['value'])
            classification(rule)
    seen = set()
    for rule in errors['headers']:
        closed(rule, {'family', 'name', 'format'})
        if rule['family'] != '*' and rule['family'] not in {v['family'] for v in errors['families']}:
            raise ValueError('error header family rejected')
        name = rule['name']
        if not isinstance(name, str) or not name or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789!#$%&'*+-.^_`|~" for c in name):
            raise ValueError('error header name rejected')
        if rule['format'] not in {'seconds_or_http_date', 'milliseconds', 'duration'} or (rule['family'], name) in seen:
            raise ValueError('error header format rejected')
        seen.add((rule['family'], name))

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--errors', required=True)
    parser.add_argument('--descriptor-policy', required=True)
    parser.add_argument('--codec-defaults', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--asset', nargs=2, action='append', default=[], metavar=('NAME', 'PATH'))
    args = parser.parse_args()
    runtime, errors = load(args.runtime), load(args.errors)
    validate(runtime, errors)
    descriptor_policy, codec_defaults = load(args.descriptor_policy), load(args.codec_defaults)
    closed(codec_defaults, {'version', 'resources', 'admission'})
    if type(codec_defaults['version']) is not int or codec_defaults['version'] != 1:
        raise ValueError('codec resource version rejected')
    for group in ('resources', 'admission'):
        closed(codec_defaults[group], RESOURCES)
        for key, value in codec_defaults[group].items():
            number(value, key)
            if not value or value > codec_defaults['admission'][key]:
                raise ValueError('codec resource admission rejected')
    lines = ['#pragma once', '#include <cstdint>', '#include <string_view>', 'namespace sp::config_defaults {']
    for prefix, values in [('defaults', runtime['defaults']), ('admission', runtime['admission'])]:
        for key, value in sorted(values.items()):
            kind = 'bool' if type(value) is bool else 'double' if key == 'retry_tokens_per_second' else 'std::uint64_t'
            literal = str(value).lower() if type(value) is bool else repr(float(value)) if kind == 'double' else str(value) + 'ULL'
            lines.append(f'inline constexpr {kind} {prefix}_{key} = {literal};')
    for group in ('resources', 'admission'):
        for key, value in sorted(codec_defaults[group].items()):
            lines.append(f'inline constexpr std::uint64_t codec_{group}_{key} = {value}ULL;')
    assets = {'runtime_defaults_json': runtime, 'error_policy_json': errors,
              'descriptor_policy_json': descriptor_policy, 'codec_defaults_json': codec_defaults}
    keywords = set(('alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t '
                    'char16_t char32_t class compl concept const consteval constexpr constinit const_cast '
                    'continue co_await co_return co_yield decltype default delete do double dynamic_cast '
                    'else enum explicit export extern false float for friend goto if inline int long '
                    'mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected '
                    'public register reinterpret_cast requires return short signed sizeof static static_assert '
                    'static_cast struct switch template this thread_local throw true try typedef typeid '
                    'typename union unsigned using virtual void volatile wchar_t while xor xor_eq').split())
    reserved = {prefix + '_' + key for prefix, values in [('defaults', runtime['defaults']),
                ('admission', runtime['admission'])] for key in values} | keywords
    reserved |= {f'codec_{group}_{key}' for group in ('resources', 'admission') for key in RESOURCES}
    for name, path in args.asset:
        if not name or not name.isascii() or not name.isidentifier() or name.startswith('_') or name in assets or name in reserved:
            raise ValueError('configuration asset symbol rejected')
        assets[name] = load(path)
    for name, value in sorted(assets.items()):
        encoded = json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False)
        lines.append(f'inline constexpr std::string_view {name} = {json.dumps(encoded)};')
    lines += ['}', '']
    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    content = '\n'.join(lines)
    if not destination.exists() or destination.read_text(encoding='utf-8') != content:
        destination.write_text(content, encoding='utf-8')

if __name__ == '__main__':
    main()

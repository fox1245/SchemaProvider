#!/usr/bin/env python3
"""Forecast qualification from measured usage; never dispatch or authorize spend."""
import argparse
import json
from decimal import Decimal
from pathlib import Path


class InputError(Exception):
    pass


def reject_duplicates(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise InputError()
        value[key] = item
    return value


def document(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"),
                          object_pairs_hook=reject_duplicates,
                          parse_constant=lambda _: (_ for _ in ()).throw(InputError()))
    except (OSError, UnicodeError, ValueError):
        raise InputError() from None


def closed(value, fields):
    if not isinstance(value, dict) or set(value) != set(fields):
        raise InputError()


def count(value, nullable=False):
    if value is None and nullable:
        return None
    if type(value) is not int or value < 0 or value > (1 << 64) - 1:
        raise InputError()
    return Decimal(value)


def fraction(value):
    if type(value) not in (int, float) or not 0 <= value <= 1:
        raise InputError()
    return Decimal(str(value))


def mean(values):
    if not values:
        raise InputError()
    return sum(values) / Decimal(len(values))


def dollars(model, input_tokens, output_tokens, read_fraction):
    input_multiplier = output_multiplier = Decimal(1)
    threshold = model["long_context_threshold"]
    if threshold is not None and input_tokens > threshold:
        input_multiplier = Decimal(str(model["long_context_input_multiplier"]))
        output_multiplier = Decimal(str(model["long_context_output_multiplier"]))
    # A forecast does not assume cache writes or treat their price as an additive fee.
    input_cost = input_tokens * ((1 - read_fraction) * model["input_rate"] +
                                read_fraction * model["cache_read_rate"]) * input_multiplier
    output_cost = output_tokens * model["output_rate"] * output_multiplier
    return input_cost / Decimal(10**12), output_cost / Decimal(10**12)


def money(value):
    return format(value, ".9f")


def calculate(catalog, plan, observations):
    closed(catalog, ("version", "currency", "rate_unit", "verified_at", "models"))
    closed(plan, ("version", "paired_runs_per_family", "baseline_cohort", "baseline_cases",
                  "models", "forecast_cache_read_fraction", "cache_sensitivity_fractions",
                  "diagnostic_requests", "diagnostic_families", "missing_output_estimate", "notes"))
    closed(observations, ("version", "sources"))
    if catalog["version"] != 1 or plan["version"] != 1 or observations["version"] != 1:
        raise InputError()
    if catalog["currency"] != "USD" or catalog["rate_unit"] != "micro_usd_per_million_tokens":
        raise InputError()
    if plan["missing_output_estimate"] != "same_family_reported_visible_output_mean":
        raise InputError()
    pairs = count(plan["paired_runs_per_family"])
    diagnostic_requests = count(plan["diagnostic_requests"])
    if not pairs or len(set(plan["baseline_cases"])) != len(plan["baseline_cases"]):
        raise InputError()
    sensitivity = [fraction(value) for value in plan["cache_sensitivity_fractions"]]
    fixed_cache = plan["forecast_cache_read_fraction"]
    if fixed_cache is not None:
        fixed_cache = fraction(fixed_cache)
    sources = observations["sources"]
    family_results = []
    total_input = total_output = Decimal(0)
    for family, model_id in plan["models"].items():
        model = catalog["models"][model_id]
        if family not in model["families"] or model["output_includes_thinking"] is not True:
            raise InputError()
        for field in ("input_rate", "cache_read_rate", "output_rate", "max_input_tokens", "max_output_tokens"):
            count(model[field])
        selected = [source for source in sources if source["api_family"] == family and
                    source["cohort"] == plan["baseline_cohort"] and source["test_only"] is False]
        if len(selected) != 1:
            raise InputError()
        rows = [row for row in selected[0]["cases"] if row["name"] in plan["baseline_cases"]]
        if {row["name"] for row in rows} != set(plan["baseline_cases"]) or len(rows) != len(plan["baseline_cases"]):
            raise InputError()
        if any(row["state"] != "passed" or row["usage_quality"] != "consistent" for row in rows):
            raise InputError()
        visible = [count(row["output_tokens"]) - count(row["reasoning"])
                   for row in rows if row["output_tokens"] is not None and row["reasoning"] is not None]
        inferred_output_rows = []
        outputs = []
        for row in rows:
            value = count(row["output_tokens"], True)
            if value is None:
                if row["reasoning_requested"] or not row["reasoning_disabled"]:
                    raise InputError()
                value = mean(visible)
                inferred_output_rows.append(row["name"])
            outputs.append(value)
        inputs = [count(row["input_tokens"]) for row in rows]
        minimum = model["cache_minimum_tokens"]
        eligible = max(inputs) >= count(minimum) if minimum is not None else None
        if fixed_cache is not None and fixed_cache > 0 and eligible is False:
            raise InputError()
        if fixed_cache is not None:
            cache = fixed_cache
            cache_basis = "configured_forecast_not_measurement"
        elif all(row["cache_read"] is not None for row in rows):
            cache = sum(count(row["cache_read"]) for row in rows) / sum(inputs)
            cache_basis = "baseline_reported_token_fraction"
        else:
            cache = Decimal(0)
            cache_basis = "no_discount_assumed_missing_cache_usage"
        calls = 2 * pairs
        input_cost, output_cost = (sum(values) * calls / Decimal(len(rows))
                                  for values in zip(*(dollars(model, i, o, cache) for i, o in zip(inputs, outputs))))
        total_input += input_cost
        total_output += output_cost
        all_rows = [row for source in sources if source["api_family"] == family and source["test_only"] is False
                    for row in source["cases"]]
        cache_rows = [row for row in all_rows if row["input_tokens"] is not None and row["cache_read"] is not None]
        observed_cache = (sum(count(row["cache_read"]) for row in cache_rows) /
                          sum(count(row["input_tokens"]) for row in cache_rows)) if cache_rows else None
        family_results.append({
            "api_family": family, "model": model_id, "calls": int(calls),
            "measured_baseline_cases": len(rows), "mean_input_tokens": float(mean(inputs)),
            "mean_output_tokens_including_thinking": float(mean(outputs)),
            "output_estimated_cases": inferred_output_rows,
            "output_estimate_basis": "same-family reported total minus reported thinking; not actual missing usage",
            "expected_input_tokens": float(mean(inputs) * calls), "expected_output_tokens": float(mean(outputs) * calls),
            "forecast_cache_read_fraction": float(cache), "forecast_cache_basis": cache_basis,
            "cache_minimum_tokens": minimum, "baseline_meets_documented_cache_minimum": eligible,
            "observed_cache_read_fraction_known_rows": float(observed_cache) if observed_cache is not None else None,
            "observed_cache_rows": len(cache_rows), "observed_input_rows": sum(row["input_tokens"] is not None for row in all_rows),
            "observed_cache_hit_requests": sum(row["cache_read"] > 0 for row in cache_rows),
            "observed_cache_hit_request_fraction_known_rows": sum(row["cache_read"] > 0 for row in cache_rows) / len(cache_rows) if cache_rows else None,
            "input_usd": money(input_cost), "output_usd": money(output_cost), "expected_usd": money(input_cost + output_cost),
            "cache_sensitivity_usd": [{"fraction": float(f),
                                      "applicability": "uncached" if f == 0 else "not_eligible_at_observed_lengths" if eligible is False else "hypothetical_not_guaranteed",
                                      "usd": money(sum(sum(dollars(model, i, o, f)) for i, o in zip(inputs, outputs)) * calls / len(rows))}
                                     for f in sensitivity],
        })
    diagnostic_rows = [row for source in sources if source["api_family"] in plan["diagnostic_families"]
                       and source["test_only"] is False for row in source["cases"]]
    diagnostic_input = max(count(row["input_tokens"]) for row in diagnostic_rows if row["input_tokens"] is not None)
    diagnostic_output = max(count(row["output_tokens"]) for row in diagnostic_rows if row["output_tokens"] is not None)
    diagnostic_cost = max(sum(dollars(catalog["models"][plan["models"][family]], diagnostic_input, diagnostic_output, Decimal(0)))
                          for family in plan["diagnostic_families"]) * diagnostic_requests
    return {"version": 1, "purpose": "workload_forecast_not_invoice_or_authorization",
            "catalog_verified_at": catalog["verified_at"], "families": family_results,
            "generation_calls": sum(row["calls"] for row in family_results),
            "expected_input_tokens": sum(row["expected_input_tokens"] for row in family_results),
            "expected_output_tokens_including_thinking": sum(row["expected_output_tokens"] for row in family_results),
            "generation_input_usd": money(total_input), "generation_output_usd": money(total_output),
            "generation_expected_usd": money(total_input + total_output),
            "diagnostic_requests": int(diagnostic_requests), "diagnostic_pricing": "largest_observed_eligible_input_and_output_without_cache_discount",
            "diagnostic_input_tokens": int(diagnostic_input), "diagnostic_output_tokens": int(diagnostic_output),
            "diagnostic_usd": money(diagnostic_cost), "combined_expected_usd": money(total_input + total_output + diagnostic_cost),
            "limits": ["Small historical cohorts are not statistical qualification.",
                       "Missing usage stays missing; only forecast fields use labelled estimates.",
                       "Reported output includes thinking; never add thinking twice.",
                       "Context window and max_output_tokens are not expected token counts.",
                       "No cached-token savings are assumed without baseline evidence.",
                       "This tool does not refund reservations, reset a ledger, or grant calls."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True)
    parser.add_argument("--plan", required=True)
    parser.add_argument("--observations", required=True)
    args = parser.parse_args()
    try:
        result = calculate(document(args.catalog), document(args.plan), document(args.observations))
    except (InputError, KeyError, TypeError, ArithmeticError):
        raise SystemExit("invalid or insufficient cost input") from None
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()

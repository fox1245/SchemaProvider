#pragma once

#include "qualification/catalog.h"
#include <memory>

namespace sp::qualification {
struct Binding {
  // SHA256 lowercase hex. Unique client/request attempt ID, NOT a vendor key.
  std::string client_request_id, fingerprint, model, family;
  std::uint64_t input_bound{}, output_bound{};
};
struct Outcome {
  bool final_consistent = false;
  Usage usage;
  bool operator==(const Outcome&) const = default;
};
struct Totals {
  std::uint64_t calls{}, spent_micro_usd{}, held_micro_usd{};
  std::uint64_t exact_settlements{}, upper_bound_settlements{}, unknown_settlements{};
  std::uint64_t call_limit = 630, money_limit_micro_usd = 1000000;
  std::uint64_t baseline_vision_calls = 99, baseline_vision_exposure_micro_usd = 18979680;
  std::uint64_t original_call_limit = 630, original_money_limit_micro_usd = 1000000;
  std::uint64_t extension_authorization_events{}, extension_calls{}, extension_micro_usd{};
};
enum class SettlementKind { UnknownHold, Exact, UpperBound };
struct Settlement {
  Totals totals;
  SettlementKind kind = SettlementKind::UnknownHold;
  std::uint64_t charged_micro_usd{};
};
class Meter;
class Claim {
 public:
  Claim(const Claim&) = delete;
  Claim& operator=(const Claim&) = delete;
  Claim(Claim&&) noexcept;
  Claim& operator=(Claim&&) noexcept;
  ~Claim() = default; // Abandonment retains the durable hold, never refunds.
  const Binding& binding() const noexcept { return binding_; }
  std::uint64_t reserved_micro_usd() const noexcept { return reserved_; }
 private:
  friend class Meter;
  Claim(std::string grant, std::uint64_t sequence, Binding binding, std::uint64_t reserved);
  std::string grant_;
  std::uint64_t sequence_{}, reserved_{};
  Binding binding_;
};
class Meter {
 public:
  // Fixed approved paths only. The authority is this approved campaign, not a
  // request profile. Existing empty/partial storage is NEVER initialized.
  // Local owner-trusted storage, not an invoice guarantee or a tamper-resistant
  // external counter. Deliberate owner rollback/deletion/copy of ALL baseline,
  // activation, grant, and meter files is outside protection. The fixed private
  // activation anchor blocks ordinary reopen/reload/path replacement resets.
  // The optional owner-approved extension is one durable authorization event in
  // that same ledger, never a replacement grant. Its declaration remains pinned
  // by exact bytes and file identity; removal/replacement fails closed.
  static Result<std::unique_ptr<Meter>> open(const std::string& project_root);
  ~Meter();
  Meter(const Meter&) = delete;
  Meter& operator=(const Meter&) = delete;
  Result<Claim> reserve(Binding binding);
  // Identical settlement is idempotent; any different second outcome fails.
  // Unknown/inconsistent usage finalizes as an unreleased hold.
  // Exact/UpperBound are catalogue meter debits, not provider invoice facts.
  Result<Settlement> settle(const Claim&, const Outcome&);
  Result<Totals> totals() const;
  const Catalog& catalog() const noexcept;
 private:
  struct Impl;
  explicit Meter(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
}  // namespace sp::qualification

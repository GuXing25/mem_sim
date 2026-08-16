#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hbm_sim/controller/timing.hpp"
#include "hbm_sim/dram/spec.hpp"

namespace {

using hbm_sim::Command;
using hbm_sim::Cycle;
using hbm_sim::DecodedAddress;
using hbm_sim::DramSpec;
using hbm_sim::TimingConstraint;
using hbm_sim::TimingEngine;
using hbm_sim::TimingScope;

struct Evidence {
  std::string standard;
  std::string profile;
  std::size_t constraint_index = 0;
  std::string parameter;
  std::string source;
  std::string reference;
  std::string scope;
  std::string preceding;
  std::string following;
  int latency_nck = 0;
  int tick_multiplier = 1;
  Cycle t_minus_1_tick = 0;
  Cycle at_t_tick = 0;
  bool t_minus_1_allowed = false;
  bool last_internal_tick_allowed = false;
  bool at_t_allowed = false;
  bool other_scope_allowed = false;
  std::string result;
};

std::string csv_escape(const std::string& value) {
  if (value.find_first_of(",\"\n") == std::string::npos) {
    return value;
  }
  std::string escaped = "\"";
  for (char ch : value) {
    if (ch == '"') escaped += '"';
    escaped += ch;
  }
  escaped += '"';
  return escaped;
}

std::string timing_source(const DramSpec& spec, const std::string& parameter) {
  auto it = std::find_if(spec.timing_table.entries.begin(), spec.timing_table.entries.end(),
                         [&](const hbm_sim::TimingTableEntry& entry) {
                           return entry.name == parameter;
                         });
  if (it != spec.timing_table.entries.end()) {
    return hbm_sim::to_string(it->source);
  }
  return "derived_expression";
}

std::string standard_reference(const DramSpec& spec) {
  if (spec.name == "HBM4") return "JESD270-4:2025; exact clause binding pending";
  if (spec.name == "LPDDR6") return "JESD209-6:2025; exact clause binding pending";
  return spec.name + " project profile; exact JEDEC clause binding pending";
}

DecodedAddress base_address() {
  DecodedAddress decoded;
  decoded.channel = 0;
  decoded.pseudo_channel = 0;
  decoded.sid = 0;
  decoded.rank = 0;
  decoded.bank_group = 0;
  decoded.bank = 0;
  decoded.row = 0;
  decoded.column = 0;
  return decoded;
}

DecodedAddress other_scope_address(DecodedAddress decoded, TimingScope scope) {
  switch (scope) {
    case TimingScope::Channel: decoded.channel = 1; break;
    case TimingScope::PseudoChannel: decoded.pseudo_channel = 1; break;
    case TimingScope::Sid: decoded.sid = 1; break;
    case TimingScope::Rank: decoded.rank = 1; break;
    case TimingScope::BankGroup: decoded.bank_group = 1; break;
    case TimingScope::Bank: decoded.bank = 1; break;
  }
  return decoded;
}

void expand_scope_dimensions(DramSpec& spec) {
  spec.org.channels = std::max(2, spec.org.channels);
  spec.org.pseudo_channels = std::max(2, spec.org.pseudo_channels);
  spec.org.sids = std::max(2, spec.org.sids);
  spec.org.ranks = std::max(2, spec.org.ranks);
  spec.org.bank_groups = std::max(2, spec.org.bank_groups);
  spec.org.banks_per_group = std::max(2, spec.org.banks_per_group);
}

Evidence check_pair(const DramSpec& original,
                    const TimingConstraint& constraint,
                    std::size_t index,
                    Command preceding,
                    Command following) {
  DramSpec spec = original;
  expand_scope_dimensions(spec);
  spec.timing_constraints = {constraint};
  TimingEngine engine(spec);
  const DecodedAddress decoded = base_address();
  const DecodedAddress other = other_scope_address(decoded, constraint.scope);
  constexpr Cycle kBase = 1000;
  const Cycle multiplier = static_cast<Cycle>(std::max(1, spec.tick_multiplier));
  const Cycle delay = static_cast<Cycle>(std::max(0, constraint.latency)) * multiplier;
  const Cycle ready = kBase + delay;
  const Cycle nck_minus_1 = constraint.latency > 0 ? ready - multiplier : kBase;
  const Cycle last_internal_tick = delay > 0 ? ready - 1 : kBase;

  engine.apply_constraints(spec, decoded, preceding, kBase);
  const bool at_t = engine.constraint_ready(spec, decoded, following, ready);
  const bool at_t_minus_1 = constraint.latency <= 0
                                ? true
                                : engine.constraint_ready(spec, decoded, following, nck_minus_1);
  const bool at_last_internal = constraint.latency <= 0
                                    ? true
                                    : engine.constraint_ready(spec, decoded, following, last_internal_tick);
  const bool other_ready = engine.constraint_ready(spec, other, following, nck_minus_1);
  const bool passed = at_t && other_ready &&
                      (constraint.latency <= 0 || (!at_t_minus_1 && !at_last_internal));

  return Evidence{
      spec.name,
      spec.timing_profile,
      index,
      constraint.parameter,
      timing_source(spec, constraint.parameter),
      standard_reference(spec),
      hbm_sim::to_string(constraint.scope),
      hbm_sim::to_string(preceding),
      hbm_sim::to_string(following),
      constraint.latency,
      spec.tick_multiplier,
      nck_minus_1,
      ready,
      at_t_minus_1,
      at_last_internal,
      at_t,
      other_ready,
      passed ? "PASS" : "FAIL",
  };
}

Evidence check_faw(const DramSpec& original,
                   const TimingConstraint& constraint,
                   std::size_t index) {
  DramSpec spec = original;
  expand_scope_dimensions(spec);
  spec.timing.nFAW = constraint.latency;
  TimingEngine engine(spec);
  const DecodedAddress decoded = base_address();
  const DecodedAddress other = other_scope_address(decoded, spec.activation_scope);
  constexpr Cycle kBase = 1000;
  const Cycle multiplier = static_cast<Cycle>(std::max(1, spec.tick_multiplier));
  const Cycle ready = kBase + static_cast<Cycle>(std::max(0, constraint.latency)) * multiplier;
  const Cycle nck_minus_1 = constraint.latency > 0 ? ready - multiplier : kBase;
  const Cycle last_internal_tick = ready > kBase ? ready - 1 : kBase;

  for (int i = 0; i < constraint.window; ++i) {
    engine.record_activate(spec, decoded, kBase);
  }
  const bool blocked_after_four = !engine.faw_ready(spec, decoded);
  engine.prune_recent_acts(spec, nck_minus_1);
  const bool blocked_at_t_minus_1 = !engine.faw_ready(spec, decoded);
  engine.prune_recent_acts(spec, last_internal_tick);
  const bool blocked_at_last_internal = !engine.faw_ready(spec, decoded);
  engine.prune_recent_acts(spec, ready);
  const bool allowed_at_t = engine.faw_ready(spec, decoded);
  const bool other_ready = engine.faw_ready(spec, other);
  const bool passed = blocked_after_four && blocked_at_t_minus_1 &&
                      blocked_at_last_internal && allowed_at_t && other_ready;

  return Evidence{
      spec.name,
      spec.timing_profile,
      index,
      constraint.parameter,
      timing_source(spec, constraint.parameter),
      standard_reference(spec),
      hbm_sim::to_string(spec.activation_scope),
      hbm_sim::to_string(constraint.preceding.front()),
      hbm_sim::to_string(constraint.following.front()),
      constraint.latency,
      spec.tick_multiplier,
      nck_minus_1,
      ready,
      !blocked_at_t_minus_1,
      !blocked_at_last_internal,
      allowed_at_t,
      other_ready,
      passed ? "PASS" : "FAIL",
  };
}

void write_csv(const std::filesystem::path& path, const std::vector<Evidence>& rows) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path);
  if (!out) throw std::runtime_error("failed to open timing evidence CSV: " + path.string());
  out << "standard,profile,constraint_index,parameter,source,reference,scope,preceding,following,"
         "latency_nck,tick_multiplier,t_minus_1_tick,at_t_tick,t_minus_1_allowed,"
         "last_internal_tick_allowed,at_t_allowed,other_scope_allowed,result\n";
  for (const auto& row : rows) {
    out << csv_escape(row.standard) << ',' << csv_escape(row.profile) << ','
        << row.constraint_index << ',' << csv_escape(row.parameter) << ','
        << csv_escape(row.source) << ',' << csv_escape(row.reference) << ','
        << csv_escape(row.scope) << ',' << csv_escape(row.preceding) << ','
        << csv_escape(row.following) << ',' << row.latency_nck << ','
        << row.tick_multiplier << ',' << row.t_minus_1_tick << ',' << row.at_t_tick << ','
        << (row.t_minus_1_allowed ? "true" : "false") << ','
        << (row.last_internal_tick_allowed ? "true" : "false") << ','
        << (row.at_t_allowed ? "true" : "false") << ','
        << (row.other_scope_allowed ? "true" : "false") << ',' << row.result << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    std::filesystem::path csv_out;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--csv-out" && i + 1 < argc) {
        csv_out = argv[++i];
      } else {
        throw std::invalid_argument("usage: timing_boundary_tests [--csv-out PATH]");
      }
    }

    std::vector<Evidence> evidence;
    std::size_t skipped_zero_latency = 0;
    for (const std::string& standard : {"hbm3", "hbm4", "lpddr5", "lpddr6"}) {
      const DramSpec spec = hbm_sim::make_spec(standard);
      for (std::size_t index = 0; index < spec.timing_constraints.size(); ++index) {
        const TimingConstraint& constraint = spec.timing_constraints[index];
        if (constraint.parameter.empty()) {
          throw std::runtime_error(spec.name + " constraint " + std::to_string(index) +
                                   " has no auditable parameter name");
        }
        if (constraint.latency <= 0) {
          ++skipped_zero_latency;
          continue;
        }
        if (constraint.window > 0) {
          evidence.push_back(check_faw(spec, constraint, index));
          continue;
        }
        for (Command preceding : constraint.preceding) {
          for (Command following : constraint.following) {
            evidence.push_back(check_pair(spec, constraint, index, preceding, following));
          }
        }
      }
    }

    if (!csv_out.empty()) write_csv(csv_out, evidence);
    const std::size_t failed = static_cast<std::size_t>(
        std::count_if(evidence.begin(), evidence.end(), [](const Evidence& row) {
          return row.result != "PASS";
        }));
    std::cout << "timing boundary matrix: " << (evidence.size() - failed) << '/'
              << evidence.size() << " passed; zero-latency/non-applicable constraints skipped="
              << skipped_zero_latency << '\n';
    return failed == 0 ? 0 : 1;
  } catch (const std::exception& ex) {
    std::cerr << "timing boundary validation failed: " << ex.what() << '\n';
    return 1;
  }
}

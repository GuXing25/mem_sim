#pragma once

#include <string>
#include <utility>
#include <vector>
#include "hbm_sim/dram/spec.hpp"

namespace hbm_sim::config {
using ModelOverrides = std::vector<std::pair<std::string, std::string>>;
struct ParameterDerivation {
  std::string key;
  std::string value;
  std::string formula;
};
struct ResolvedModelInputs {
  ModelOverrides overrides;
  std::vector<ParameterDerivation> derived;
};

// Schema 3 derives redundant inputs, but never synthesizes a vendor RL/WL table.
// The caller supplies a protocol baseline; explicit dependent values are checked
// against their determinants. Omission and 'auto' request derivation, not zero.
ResolvedModelInputs resolve_coupled_inputs(const DramSpec& baseline,
                                         const ModelOverrides& inputs);

// Input keys are canonical (lowercase, underscores); vector order preserves
// layered overrides and the legacy timing-source scope. Unknown keys fail.
bool is_spec_override_key(const std::string& key);
void apply_spec_overrides(DramSpec& spec, const ModelOverrides& overrides,
                          bool resolved_clock = false);
DramSpec build_model(const std::string& standard,
                     const ModelOverrides& overrides = {},
                     int schema_version = 2,
                     std::vector<ParameterDerivation>* derived = nullptr);
}  // namespace hbm_sim::config

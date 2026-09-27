// Rack Capacity - human-authorable specification text and report rendering.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>
#include <string_view>

#include "rack_capacity/capacity.hpp"
#include "rack_capacity/evidence.hpp"
#include "rack_capacity/export.hpp"
#include "rack_capacity/fit.hpp"
#include "rack_capacity/ids.hpp"
#include "rack_capacity/result.hpp"

namespace rackcapacity {

// ---------------------------------------------------------------------------
// Specification text
// ---------------------------------------------------------------------------
//
// The specification format exists so that a capacity evidence bundle can be
// authored, reviewed and diffed by a person, and so the command line tool can
// apply one without a second parser dependency. It is line oriented:
//
//   rcap-spec 1
//   [composition]
//   rack = rack-a1
//   generation = 4
//   units = 48
//   structural-reserved = [1,3),[95,97)
//   front-clearance-mm = 900
//   rear-clearance-mm = 700
//   service-height-limit-u = 42
//   site = dc1
//   [policy]
//   policy = standard-2026
//   policy-version = 3
//   power-headroom-w = 500
//   cooling-headroom-w = 500
//   weight-headroom-g = 20000
//   slot-headroom = 2
//   power-derate-bp = 500
//   cooling-derate-bp = 500
//   weight-derate-bp = 0
//   max-envelope-age-ns = 3600000000000
//   max-measurement-age-ns = 300000000000
//   heat-per-power-ppm = 1000000
//   [power]
//   feeds = 2
//   watts-per-feed = 8000
//   redundancy = n2
//   derate-bp = 2000
//   measured-draw-w = 4100
//   measured-at-ns = 1767225600000000000
//   [cooling]
//   nominal-heat-rejection-w = 20000
//   derate-bp = 1000
//   supply-air-temp-mc = 24000
//   [weight]
//   static-load-limit-g = 900000
//   derate-bp = 0
//   per-unit-point-load-limit-g = 250000
//   [asset a-0001]
//   span = [1,5)
//   kind = full
//   presence = present
//   nameplate-draw-w = 1200
//   declared-heat-w = 1200
//   mass-g = 32000
//   source = asset-registry
//   evidence = ev-a-0001
//   version = 7
//   observed-at-ns = 1767225600000000000
//   [reservation r-0001]
//   state = committed
//   span = [21,23)
//   kind = full
//   draw-w = 800
//   ...
//   [request]
//   epoch = 12
//   captured-at-ns = 1767225600000000000
//   actor = operator-1
//   request = req-0001
//
// Unknown keys, duplicate keys, duplicate sections, out-of-range values and
// malformed text are all rejected with a line number. Nothing is normalized and
// nothing is silently defaulted: a missing mandatory key is an error, not a
// zero.
struct SpecificationParseOptions {
  // Source label used in error messages, typically the file name.
  std::string source_name{};
  // When true, an empty input is an error rather than an empty bundle.
  bool require_content = true;
};

[[nodiscard]] RACK_CAPACITY_API Result<RackCapacityInputs> parse_specification(
    std::string_view text, const SpecificationParseOptions& options = {});

// Renders a bundle in the specification format, so that what was applied can be
// read back and diffed. The rendering round-trips through parse_specification.
[[nodiscard]] RACK_CAPACITY_API std::string render_specification(
    const RackCapacityInputs& inputs);

}  // namespace rackcapacity

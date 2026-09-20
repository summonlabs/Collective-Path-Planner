// Collective Path Planner - version and build identification.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "cpath/version.hpp"

namespace cpath {

const char* version_string() noexcept { return kVersionString; }

std::string build_summary() {
  std::string summary;
  summary.reserve(128);
  summary += kProjectName;
  summary += ' ';
  summary += kVersionString;
  summary += " [";
  summary += (kBuildType[0] != '\0') ? kBuildType : "unspecified";
  summary += ", ";
  summary += kCompilerId;
  summary += ' ';
  summary += kCompilerVersion;
  summary += ", sanitizers=";
  summary += kSanitizersEnabled ? "on" : "off";
  summary += ", store-format=";
  summary += std::to_string(kStoreFormatVersion);
  summary += ", canonical-format=";
  summary += std::to_string(kCanonicalFormatVersion);
  summary += ']';
  return summary;
}

}  // namespace cpath

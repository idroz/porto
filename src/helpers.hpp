#include <cgroup.hpp>
#include <string>
#include <vector>
#include "util/path.hpp"

TError RunCommand(const std::vector<TString> &command,
                  const TFile &dir = TFile(),
                  const TFile &input = TFile(),
                  const TFile &output = TFile(),
                  const TCapabilities &caps = HelperCapabilities);
TError CopyRecursive(const TPath &src, const TPath &dst);
TError ClearRecursive(const TPath &path);
TError RemoveRecursive(const TPath &path);

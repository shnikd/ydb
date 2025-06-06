#pragma clang system_header
// Copyright 2021 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_PROFILE_MARSHALER_H_
#define TCMALLOC_PROFILE_MARSHALER_H_

#include <string>

#include "absl/status/statusor.h"
#include "tcmalloc/malloc_extension.h"

namespace tcmalloc {

// Marshal converts a Profile instance into a gzip-encoded, serialized
// representation suitable for viewing with PProf
// (https://github.com/google/pprof).
absl::StatusOr<std::string> Marshal(const tcmalloc::Profile& profile);

}  // namespace tcmalloc

#endif  // TCMALLOC_PROFILE_MARSHALER_H_

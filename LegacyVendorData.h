/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>

namespace android::vold {

// Restrict the command-line migration surface to legacy data and vendor data.
bool IsLegacyVendorMigrationPathPair(const std::string& source, const std::string& destination);

// Run before the directory's producers start. Rename retains contents, inode
// identity, ownership, modes and xattrs on the same filesystem; conflicting directories
// remain separate. A missing destination starts root-owned with mode 0700,
// and init must apply the destination's owner and mode before starting users.
// A retry after rename completes the source link without moving data again.
bool MigrateLegacyVendorDirectory(const std::string& source, const std::string& destination,
                                  std::string* error);

}  // namespace android::vold

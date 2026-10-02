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

#include "LegacyVendorData.h"

#include <android-base/unique_fd.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <vector>

namespace android::vold {
namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
}

bool FailErrno(std::string* error, const std::string& operation) {
    return Fail(error, operation + ": " + std::strerror(errno));
}

bool SplitPath(const std::string& path, std::vector<std::string>* components) {
    if (path.empty() || path.front() != '/' || path.back() == '/') return false;
    size_t start = 1;
    while (start < path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string component = path.substr(start, end - start);
        if (component.empty() || component == "." || component == "..") return false;
        components->push_back(component);
        start = end + 1;
    }
    return !components->empty();
}

android::base::unique_fd OpenParent(const std::vector<std::string>& components) {
    // Ancestors need path search; only the final parent needs a syncable FD.
    const int root_mode = components.size() == 1 ? O_RDONLY : O_PATH;
    android::base::unique_fd directory(open("/", root_mode | O_DIRECTORY | O_CLOEXEC));
    for (size_t index = 0; directory.ok() && index + 1 < components.size(); ++index) {
        const int mode = index + 2 == components.size() ? O_RDONLY : O_PATH;
        directory.reset(openat(directory.get(), components[index].c_str(),
                               mode | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    }
    return directory;
}

bool StatEntry(int parent, const std::string& name, struct stat* metadata, bool* exists) {
    if (fstatat(parent, name.c_str(), metadata, AT_SYMLINK_NOFOLLOW) == 0) {
        *exists = true;
        return true;
    }
    if (errno != ENOENT) return false;
    *exists = false;
    return true;
}

bool HasDestination(int parent, const std::string& name, const std::string& destination) {
    std::vector<char> buffer(destination.size() + 1);
    ssize_t length = readlinkat(parent, name.c_str(), buffer.data(), buffer.size());
    return length == static_cast<ssize_t>(destination.size()) &&
           std::memcmp(buffer.data(), destination.data(), destination.size()) == 0;
}

bool IsAncestor(const std::string& parent, const std::string& child) {
    return child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 &&
           child[parent.size()] == '/';
}

template <typename Integer>
bool ParseUnsigned(const std::string& input, int base, Integer maximum, Integer* value) {
    Integer parsed = 0;
    const auto result = std::from_chars(input.data(), input.data() + input.size(), parsed, base);
    if (result.ec != std::errc() || result.ptr != input.data() + input.size() || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

bool SetOwnershipAndMode(int descriptor, const LegacyVendorOwnership& ownership,
                         std::string* error) {
    if (fchown(descriptor, ownership.uid, ownership.gid) != 0 ||
        fchmod(descriptor, ownership.mode) != 0) {
        return FailErrno(error, "set prepared directory metadata");
    }
    struct stat metadata = {};
    if (fstat(descriptor, &metadata) != 0) return FailErrno(error, "stat prepared directory");
    // chmod may succeed after clearing setgid when CAP_FSETID is absent.
    if (metadata.st_uid != ownership.uid || metadata.st_gid != ownership.gid ||
        (metadata.st_mode & 07777) != ownership.mode) {
        return Fail(error, "prepared directory differs from requested mode or owner");
    }
    if (fsync(descriptor) != 0) return FailErrno(error, "sync prepared directory metadata");
    return true;
}

}  // namespace

bool IsLegacyVendorMigrationPathPair(const std::string& source, const std::string& destination) {
    std::vector<std::string> source_components;
    std::vector<std::string> destination_components;
    return SplitPath(source, &source_components) &&
           SplitPath(destination, &destination_components) && source_components.size() >= 2 &&
           source_components[0] == "data" && source_components[1] != "vendor" &&
           destination_components.size() >= 3 && destination_components[0] == "data" &&
           destination_components[1] == "vendor";
}

bool ParseLegacyVendorOwnership(const std::string& mode, const std::string& uid,
                                const std::string& gid, LegacyVendorOwnership* ownership) {
    LegacyVendorOwnership parsed = {};
    if (!ParseUnsigned(mode, 8, static_cast<mode_t>(07777), &parsed.mode) ||
        !ParseUnsigned(uid, 10, static_cast<uid_t>(std::numeric_limits<uid_t>::max() - 1),
                       &parsed.uid) ||
        !ParseUnsigned(gid, 10, static_cast<gid_t>(std::numeric_limits<gid_t>::max() - 1),
                       &parsed.gid)) {
        return false;
    }
    *ownership = parsed;
    return true;
}

bool IsLegacyVendorSubdirectoryName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

bool MigrateLegacyVendorDirectory(const std::string& source, const std::string& destination,
                                  std::string* error) {
    std::vector<std::string> source_components;
    std::vector<std::string> destination_components;
    if (!SplitPath(source, &source_components) ||
        !SplitPath(destination, &destination_components) || source == destination ||
        IsAncestor(source, destination) || IsAncestor(destination, source)) {
        return Fail(error, "migration requires separate absolute directory paths");
    }

    auto source_parent = OpenParent(source_components);
    if (!source_parent.ok()) return FailErrno(error, "open source parent");
    auto destination_parent = OpenParent(destination_components);
    if (!destination_parent.ok()) return FailErrno(error, "open destination parent");
    const std::string& source_name = source_components.back();
    const std::string& destination_name = destination_components.back();
    struct stat source_metadata = {};
    struct stat destination_metadata = {};
    struct stat destination_parent_metadata = {};
    bool source_exists = false;
    bool destination_exists = false;
    if (!StatEntry(source_parent.get(), source_name, &source_metadata, &source_exists)) {
        return FailErrno(error, "stat source");
    }
    if (!StatEntry(destination_parent.get(), destination_name, &destination_metadata,
                   &destination_exists)) {
        return FailErrno(error, "stat destination");
    }
    if (destination_exists && !S_ISDIR(destination_metadata.st_mode)) {
        return Fail(error, "destination exists without a directory");
    }
    if (source_exists && S_ISLNK(source_metadata.st_mode)) {
        if (!HasDestination(source_parent.get(), source_name, destination)) {
            return Fail(error, "source link points to a different destination");
        }
        if (!destination_exists) return Fail(error, "source link has a missing destination");
        // Retry the durability barrier if an earlier link publication failed to sync.
        if (fsync(destination_parent.get()) != 0 || fsync(source_parent.get()) != 0) {
            return FailErrno(error, "sync existing migration");
        }
        return true;
    }
    if (source_exists && !S_ISDIR(source_metadata.st_mode)) {
        return Fail(error, "source exists without a directory or matching link");
    }
    if (source_exists && destination_exists) {
        return Fail(error, "source and destination directories both exist; preserve both");
    }

    if (source_exists) {
        if (fstat(destination_parent.get(), &destination_parent_metadata) != 0) {
            return FailErrno(error, "stat destination parent");
        }
        if (source_metadata.st_dev != destination_parent_metadata.st_dev) {
            return Fail(error, "source and destination are on different filesystems");
        }
        // Producers have not started, and init owns the destination namespace.
        // The boot-time precondition permits rename on kernels without renameat2.
        if (renameat(source_parent.get(), source_name.c_str(), destination_parent.get(),
                     destination_name.c_str()) != 0) {
            return FailErrno(error, "rename source to destination");
        }
    } else if (!destination_exists &&
               mkdirat(destination_parent.get(), destination_name.c_str(), 0700) != 0) {
        return FailErrno(error, "create destination");
    }

    // Persist the destination before publishing the compatibility link. A
    // failure leaves moved data at its destination for the next boot to adopt.
    if (fsync(destination_parent.get()) != 0 || fsync(source_parent.get()) != 0) {
        return FailErrno(error, "sync directory migration");
    }
    if (symlinkat(destination.c_str(), source_parent.get(), source_name.c_str()) != 0) {
        return FailErrno(error, "create compatibility link");
    }
    if (fsync(source_parent.get()) != 0) return FailErrno(error, "sync compatibility link");
    return true;
}

bool PrepareLegacyVendorDirectory(const std::string& source, const std::string& destination,
                                  const LegacyVendorOwnership& ownership, std::string* error) {
    if (!MigrateLegacyVendorDirectory(source, destination, error)) return false;
    std::vector<std::string> components;
    if (!SplitPath(destination, &components)) return Fail(error, "invalid destination path");
    auto parent = OpenParent(components);
    if (!parent.ok()) return FailErrno(error, "open prepared destination parent");
    android::base::unique_fd directory(openat(parent.get(), components.back().c_str(),
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory.ok()) return FailErrno(error, "open prepared destination");
    // chown may clear setgid; apply the complete mode after ownership.
    return SetOwnershipAndMode(directory.get(), ownership, error);
}

bool PrepareLegacyVendorSubdirectories(const std::string& destination,
                                       const std::vector<LegacyVendorSubdirectory>& subdirectories,
                                       std::string* error) {
    for (const auto& subdirectory : subdirectories) {
        if (!IsLegacyVendorSubdirectoryName(subdirectory.name)) {
            return Fail(error, "subdirectory requires a single relative name");
        }
    }
    std::vector<std::string> components;
    if (!SplitPath(destination, &components)) return Fail(error, "invalid destination path");
    auto parent = OpenParent(components);
    if (!parent.ok()) return FailErrno(error, "open subdirectory destination parent");
    android::base::unique_fd directory(openat(parent.get(), components.back().c_str(),
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory.ok()) return FailErrno(error, "open subdirectory destination");
    for (const auto& subdirectory : subdirectories) {
        if (mkdirat(directory.get(), subdirectory.name.c_str(), 0700) != 0 && errno != EEXIST) {
            return FailErrno(error, "create prepared subdirectory " + subdirectory.name);
        }
        android::base::unique_fd child(openat(directory.get(), subdirectory.name.c_str(),
                                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!child.ok()) return FailErrno(error, "open prepared subdirectory " + subdirectory.name);
        if (!SetOwnershipAndMode(child.get(), subdirectory.ownership, error)) {
            return false;
        }
    }
    if (fsync(directory.get()) != 0) return FailErrno(error, "sync prepared subdirectories");
    return true;
}

}  // namespace android::vold

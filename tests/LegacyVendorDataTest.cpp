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

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;

void Require(bool result, const std::string& description) {
    if (!result) throw std::runtime_error(description);
}

struct stat Metadata(const fs::path& path) {
    struct stat metadata = {};
    Require(lstat(path.c_str(), &metadata) == 0, "lstat " + path.string());
    return metadata;
}

void Write(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    Require(static_cast<bool>(output << contents), "write " + path.string());
    output.close();
    Require(static_cast<bool>(output), "close " + path.string());
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "read " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void PreserveMetadata(const struct stat& before, const struct stat& after) {
    Require(before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
                    before.st_mode == after.st_mode && before.st_uid == after.st_uid &&
                    before.st_gid == after.st_gid && before.st_size == after.st_size,
            "device, inode, mode, uid/gid or size changed");
}

bool Migrate(const fs::path& source, const fs::path& destination) {
    std::string error;
    bool result = android::vold::MigrateLegacyVendorDirectory(source, destination, &error);
    if (!result) std::cout << "expected/observed rejection: " << error << '\n';
    return result;
}

fs::path Case(const fs::path& root, const std::string& name) {
    auto directory = root / name;
    Require(fs::create_directory(directory), "fresh fixture " + directory.string());
    return directory;
}

void MigrationAndLifecycle(const fs::path& root) {
    auto directory = Case(root, "contents-metadata-and-socket-lifecycle");
    auto source = directory / "legacy";
    auto destination = directory / "vendor";
    Require(fs::create_directory(source), "create source");
    Require(chmod(source.c_str(), 02770) == 0, "source directory mode");
    Write(source / "state", "persistent vendor state\n");
    Require(chmod((source / "state").c_str(), 0640) == 0, "state file mode");
    Require(setxattr((source / "state").c_str(), "user.vold_fixture", "retained", 8, 0) == 0,
            "write fixture xattr");
    Require(link((source / "state").c_str(), (directory / "hardlink").c_str()) == 0,
            "create external hardlink");
    fs::create_symlink("state", source / "relative-link");
    auto before_directory = Metadata(source);
    auto before_file = Metadata(source / "state");
    Require(Migrate(source, destination), "initial migration");
    Require(fs::is_symlink(fs::symlink_status(source)) && fs::read_symlink(source) == destination,
            "legacy alias destination");
    PreserveMetadata(before_directory, Metadata(destination));
    PreserveMetadata(before_file, Metadata(destination / "state"));
    PreserveMetadata(before_file, Metadata(directory / "hardlink"));
    Require(Read(source / "state") == "persistent vendor state\n", "content through alias");
    Require(fs::read_symlink(destination / "relative-link") == "state", "nested link retained");
    char xattr[9] = {};
    Require(getxattr((destination / "state").c_str(), "user.vold_fixture", xattr, sizeof(xattr)) ==
                            8 &&
                    std::memcmp(xattr, "retained", 8) == 0,
            "xattr retained");
    auto alias_metadata = Metadata(source);
    Require(Migrate(source, destination), "idempotent retry");
    PreserveMetadata(alias_metadata, Metadata(source));

    // mpdecision binds below the legacy directory name after relocation.
    android::base::unique_fd socket_fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    Require(socket_fd.ok(), "create lifecycle socket");
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::string socket_path = (source / "socket").string();
    Require(socket_path.size() < sizeof(address.sun_path), "fixture socket path length");
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    Require(bind(socket_fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
                    0,
            "bind socket through alias");
    Require(S_ISSOCK(Metadata(destination / "socket").st_mode), "socket lands in vendor directory");
    std::cout
            << "PASS contents, identity, ownership, modes, xattr, links, retry, socket creation\n";
}

void Conflicts(const fs::path& root) {
    auto directory = Case(root, "existing-directory-conflict");
    auto source = directory / "legacy";
    auto destination = directory / "vendor";
    fs::create_directory(source);
    fs::create_directory(destination);
    Write(source / "state", "legacy state");
    Write(destination / "state", "separate vendor state");
    auto before_source = Metadata(source / "state");
    auto before_destination = Metadata(destination / "state");
    Require(!Migrate(source, destination), "reject directory conflict");
    PreserveMetadata(before_source, Metadata(source / "state"));
    PreserveMetadata(before_destination, Metadata(destination / "state"));
    Require(Read(source / "state") == "legacy state" &&
                    Read(destination / "state") == "separate vendor state",
            "conflicting contents preserved");
    std::cout << "PASS conflicting directories retain independent contents\n";
}

void RecoveryAndFreshData(const fs::path& root) {
    auto directory = Case(root, "rename-recovery");
    auto source = directory / "legacy";
    auto destination = directory / "vendor";
    fs::create_directory(destination);
    Write(destination / "state", "moved before interruption");
    auto before = Metadata(destination / "state");
    Require(Migrate(source, destination), "recover interrupted rename");
    PreserveMetadata(before, Metadata(source / "state"));
    Require(Read(source / "state") == "moved before interruption", "recovered contents");

    directory = Case(root, "fresh-data");
    source = directory / "legacy";
    destination = directory / "vendor";
    Require(Migrate(source, destination), "initialize missing directories");
    Require((Metadata(destination).st_mode & 07777) == 0700, "secure new directory mode");
    Require(Migrate(source, destination), "retry fresh data");
    std::cout << "PASS rename interruption recovery and fresh-data retry\n";
}

void InvalidObjects(const fs::path& root) {
    auto directory = Case(root, "invalid-objects");
    fs::create_directory(directory / "actual");
    fs::create_symlink(directory / "actual", directory / "wrong-link");
    Require(!Migrate(directory / "wrong-link", directory / "vendor"), "reject wrong source link");
    Write(directory / "file", "preserve regular file");
    Require(!Migrate(directory / "file", directory / "vendor"), "reject source regular file");
    Require(!Migrate(directory / "missing", directory / "file"), "reject destination regular file");
    Require(Read(directory / "file") == "preserve regular file", "regular file preserved");
    fs::create_symlink(directory / "actual", directory / "parent-link");
    Require(!Migrate(directory / "parent-link/child", directory / "vendor"),
            "reject intermediate source link");
    fs::create_directory(directory / "source");
    Require(!Migrate(directory / "source", directory / "parent-link/child"),
            "reject intermediate destination link");
    Require(!Migrate(directory / "source", directory / "source/child"), "reject ancestry");
    Require(!Migrate(directory / "source", directory / "source"), "reject identical paths");
    Require(!Migrate(directory / "actual/../source", directory / "vendor"), "reject dot traversal");
    std::cout << "PASS conflicting links/files, intermediate links, ancestry and traversal "
                 "rejection\n";
}

void CrossFilesystem(const fs::path& root, const fs::path& other_root) {
    auto directory = Case(root, "cross-filesystem-source");
    auto other_directory = Case(other_root, "cross-filesystem-destination");
    auto source = directory / "legacy";
    auto destination = other_directory / "vendor";
    fs::create_directory(source);
    Write(source / "state", "retain on original filesystem");
    auto before_directory = Metadata(source);
    auto before_file = Metadata(source / "state");
    Require(before_directory.st_dev != Metadata(other_directory).st_dev,
            "cross-filesystem fixture requires distinct filesystems");
    Require(!Migrate(source, destination), "reject cross-filesystem migration");
    PreserveMetadata(before_directory, Metadata(source));
    PreserveMetadata(before_file, Metadata(source / "state"));
    Require(Read(source / "state") == "retain on original filesystem", "source content retained");
    Require(!fs::exists(destination), "rejected destination remains absent");
    std::cout << "PASS cross-filesystem rejection retains source contents and metadata\n";
}

void CommandPathBoundary() {
    using android::vold::IsLegacyVendorMigrationPathPair;
    Require(IsLegacyVendorMigrationPathPair("/data/misc/location", "/data/vendor/location"),
            "accept legacy-to-vendor command paths");
    for (const std::string& source :
         {"/data", "/data/vendor", "/data/vendor/location", "/system/location", "data/location",
          "/data/../system", "/data//location", "/data/location/"}) {
        Require(!IsLegacyVendorMigrationPathPair(source, "/data/vendor/location"),
                "reject source boundary " + source);
    }
    for (const std::string& destination :
         {"/data/vendor", "/data/misc/location", "/vendor/location", "/data/vendor/../location",
          "/data/vendor//location", "/data/vendor/location/"}) {
        Require(!IsLegacyVendorMigrationPathPair("/data/misc/location", destination),
                "reject destination boundary " + destination);
    }
    std::cout << "PASS command path ownership and normalization boundaries\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0]
                  << " FRESH_EXISTING_FIXTURE_ROOT FRESH_ROOT_ON_OTHER_FILESYSTEM\n";
        return 2;
    }
    try {
        fs::path root = fs::absolute(argv[1]);
        fs::path other_root = fs::absolute(argv[2]);
        Require(fs::is_directory(root) && fs::is_empty(root), "fixture root must be empty");
        Require(fs::is_directory(other_root) && fs::is_empty(other_root),
                "other filesystem fixture root must be empty");
        CommandPathBoundary();
        MigrationAndLifecycle(root);
        Conflicts(root);
        RecoveryAndFreshData(root);
        InvalidObjects(root);
        CrossFilesystem(root, other_root);
        std::cout << "Retained fixtures: " << root << '\n';
    } catch (const std::exception& error) {
        std::cerr << "ERROR " << error.what() << '\n';
        return 1;
    }
    return 0;
}

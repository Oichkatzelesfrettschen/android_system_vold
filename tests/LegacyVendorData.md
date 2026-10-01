# Legacy vendor data directory migration

`vold_prepare_subdirs migrate-vendor-data LEGACY_DIRECTORY VENDOR_DIRECTORY`
moves a legacy directory under `/data` into `/data/vendor` and publishes an
absolute compatibility symlink at its original name. Existing vendor binaries
can keep their hardcoded paths while file contexts describe partition ownership.

Init must invoke the helper before every producer of either directory starts.
Init must own both parent namespaces and exclude concurrent creation or rename
there. The helper uses `renameat`, including on kernels without `renameat2`;
the early-boot precondition prevents replacement of a concurrently created
destination. Init must apply destination ownership/mode and restorecon the
moved contents and the compatibility link before starting consumers. Init must
gate consumer startup on successful preparation; an `exec` command alone
continues the action list after the child exits.

The helper rejects parent symlinks, dot traversal, regular files, mismatched
source symlinks, cross-filesystem moves and two existing directories. Rejection
preserves conflicting contents for operator reconciliation. A rename failure,
including incompatible encryption policies, reports failure instead of copying
or deleting data. A missing source with an existing target recovers an
interrupted rename. A matching source link retries the parent fsync barriers.
New targets start with mode 0700; final ownership and permissions belong to init.

The native filesystem test uses production helper code. It measures contents,
device/inode identity, mode, uid/gid, size, a user xattr, hardlinks, relative
symlinks, retry, interrupted-rename state recovery, conflict rejection, Unix
socket creation through the compatibility link and cross-filesystem rejection.
The command-path tests reject ownership-boundary and normalization violations.
The fixture tests require two existing empty directories on different
filesystems with user xattrs and Unix sockets supported. Fixtures remain for
inspection after the test.

Build the `vold_legacy_vendor_data_test` host module and invoke its executable:

```sh
vold_legacy_vendor_data_test FRESH_FIXTURE_ROOT FRESH_ROOT_ON_OTHER_FILESYSTEM
```

A direct compiler gate can run from system/vold:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I../libbase/include -I. \
    LegacyVendorData.cpp tests/LegacyVendorDataTest.cpp -o TEST_BINARY
TEST_BINARY FRESH_FIXTURE_ROOT FRESH_ROOT_ON_OTHER_FILESYSTEM
```

Host results cover the named filesystem assertions. Android init ordering,
SELinux transitions and relabeling, encrypted-directory compatibility and
power-loss behavior require separate product and device gates.

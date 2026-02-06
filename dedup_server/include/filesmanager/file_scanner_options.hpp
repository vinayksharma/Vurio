#pragma once

namespace MediaDedup::Files
{
    // Options to control directory scanning behavior.
    struct FileScannerOptions
    {
        // If true, traverse subdirectories recursively; if false, only scan the top-level directory.
        // Default: true. May be overridden by configuration (filesservice.scan.recursive).
        bool recursive = true;

        // If true, follow directory/file symlinks during traversal (may risk cycles).
        // Default: false. With false, the scanner emits the symlink entry itself and, when resolvable,
        // records its target path without traversing into it.
        bool followSymlinks = false;

        // If true, include hidden files when emitting results (dotfiles on Unix, hidden attribute on Windows).
        // Default: true. Best-effort cross-platform behavior (may vary by platform semantics).
        bool includeHidden = true;
    };
}

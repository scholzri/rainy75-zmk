#!/bin/bash
# Apply local patches to west-managed repositories after west update.
# Usage: ./patches/apply-patches.sh
set -e
cd "$(dirname "$0")/.."

echo "=== Applying Zephyr patches ==="
for patch in patches/zephyr/*.patch; do
    echo "  Applying: $(basename "$patch")"
    git -C zephyr am --3way "$PWD/$patch" 2>/dev/null || \
        git -C zephyr am --abort 2>/dev/null && \
        echo "    (already applied or conflict — skipping)"
done
echo "Done."

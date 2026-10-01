#!/bin/bash
# robust-corpus.sh [dir]: the corpus of the robustness run - the corrupted
# file system images of the e2fsprogs test suite (tests/*/image.gz, each a
# real bug report or a hand-made corruption), unpacked to <dir>/img/<test>.img
# and listed in <dir>/list.txt as "<bytes> <test>", smallest first.
dir=${1:-~/robust}
src=$dir/e2fsprogs
if [ ! -d "$src/tests" ]; then
    git clone -q --depth 1 --sparse https://git.kernel.org/pub/scm/fs/ext2/e2fsprogs.git "$src" || exit 1
    (cd "$src" && git sparse-checkout set tests) || exit 1
fi
mkdir -p "$dir/img"
for gz in "$src"/tests/*/image.gz; do
    t=$(basename "$(dirname "$gz")")
    [ -f "$dir/img/$t.img" ] || gunzip -c "$gz" > "$dir/img/$t.img" 2>/dev/null || rm -f "$dir/img/$t.img"
done
for f in "$dir"/img/*.img; do
    printf '%s %s\n' "$(stat -c %s "$f")" "$(basename "$f" .img)"
done | sort -n > "$dir/list.txt"
echo "$(wc -l < "$dir/list.txt") images, e2fsprogs $(git -C "$src" log -1 --format=%h)"

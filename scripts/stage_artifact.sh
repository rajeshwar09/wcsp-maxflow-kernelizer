#!/usr/bin/env bash
# stage_artifact.sh
# makes a clean local copy of the published benchmark from the drive, and data/artifact/manifest.tsv that says what every file is
# steps:
#   1. finds the evalgm/ and uai/ folders under --source (up to 4 levels deep)
#   2. reads the header of every .wcsp and .uai and puts it in a class
#        boolean    : every domain is 2               -> copied, usable directly
#        domain1    : domains are only 1 and 2        -> copied, usable after wcsp_collapse
#        multi      : some domain is 3 or more        -> not copied, no CCG method can use it
#        unreadable : the header could not be read    -> not copied
#   3. writes data/artifact/manifest.tsv
#   4. copies the usable files into <dest>.staging, with the same folder layout
#   5. the old --dest folder is renamed to <dest>.old-<date> (never deleted) and the staging folder becomes --dest
#
# usage: ./scripts/stage_artifact.sh --source DIR [--dest DIR] [--dry-run]
#   --source DIR   where the drive is mounted, for example /run/media/raje/D8881E91881E6DEE
#   --dest DIR     the local copy                    (default ~/mtp/wcsp-maxflow/artifact)
#   --dry-run      only classify and write the manifest, nothing is copied or renamed
# the log goes to results/<date>/stage_artifact_<time>.log
# manifest columns: relpath, collection, family, format, nvars, class, copied, bytes (see data/artifact/manifest.legend.txt)

set -u
cd "$(dirname "$0")/.." || exit 1

# [ARGS] defaults and command line
src=""
dest=~/mtp/wcsp-maxflow/artifact
dry_run=0
while [ $# -gt 0 ]; do
  case "$1" in
    --source) src="$2"; shift 2 ;;
    --dest) dest="$2"; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 1 ;;
  esac
done
if [ -z "$src" ]; then
  echo "--source is needed (see --help)"
  exit 1
fi
if [ ! -d "$src" ]; then
  echo "source not found: $src (is the drive mounted?)"
  exit 1
fi

# [LOG] one log file per run
mkdir -p "results/$(date +%F)" data/artifact
log="results/$(date +%F)/stage_artifact_$(date +%H%M%S).log"
say() {
  echo "$*" | tee -a "$log"
}

# [FIND] the two collection folders on the drive
evalgm_root=$(find "$src" -maxdepth 4 -type d -name evalgm 2>/dev/null | head -1)
uai_root=$(find "$src" -maxdepth 4 -type d \( -name uai -o -name uai-instances \) 2>/dev/null | head -1)
say "stage artifact $(date '+%F %T')  commit $(git rev-parse --short HEAD)  dry_run $dry_run"
say "source $src"
say "evalgm ${evalgm_root:-not found}"
say "uai    ${uai_root:-not found}"
say "dest   $dest"
if [ -z "$evalgm_root" ] && [ -z "$uai_root" ]; then
  say "nothing to stage"
  exit 1
fi

manifest=data/artifact/manifest.tsv
printf 'relpath\tcollection\tfamily\tformat\tnvars\tclass\tcopied\tbytes\n' > "$manifest"

# [CLASS] .wcsp header: line 1 has the variable count in field 2, line 2 has the domains, prints "class<TAB>nvars"
# done stops the END block from printing a second line after exit
classify_wcsp() {
  awk 'NR == 1 { nv = $2; if (nv + 0 <= 0) { print "unreadable\tNA"; done = 1; exit } next }
       NR == 2 { cls = "boolean"
                 for (i = 1; i <= NF; i++) {
                   if ($i + 0 >= 3) { cls = "multi"; break }
                   if ($i + 0 == 1) cls = "domain1"
                   else if ($i + 0 != 2) { cls = "unreadable"; break }
                 }
                 print cls "\t" nv; done = 1; exit }
       END { if (!done) print "unreadable\tNA" }' "$1"
}

# [CLASS] .uai header: line 2 has the variable count, line 3 has the domains
classify_uai() {
  awk 'NR == 2 { nv = $1 + 0; if (nv <= 0) { print "unreadable\tNA"; done = 1; exit } }
       NR == 3 { cls = "boolean"
                 for (i = 1; i <= NF; i++) {
                   if ($i + 0 >= 3) { cls = "multi"; break }
                   if ($i + 0 == 1) cls = "domain1"
                   else if ($i + 0 != 2) { cls = "unreadable"; break }
                 }
                 print cls "\t" nv; done = 1; exit }
       END { if (!done) print "unreadable\tNA" }' "$1"
}

stage="${dest}.staging"
rm -rf "$stage"
[ "$dry_run" = 0 ] && mkdir -p "$stage"

# [SCAN] classify every file of one collection, copy the usable ones, add one manifest row each
scan_collection() {
  local root="$1" coll="$2" ext="$3"
  [ -n "$root" ] || return 0
  local files f rel fam cn cls nv copy i=0
  mapfile -t files < <(find "$root" -name "*.$ext" -type f | sort)
  say ""
  say "$coll: ${#files[@]} files"
  for f in "${files[@]}"; do
    i=$((i + 1))
    rel="${f#"$root"/}"
    fam=$(basename "$(dirname "$f")")
    if [ "$ext" = "wcsp" ]; then
      cn=$(classify_wcsp "$f")
    else
      cn=$(classify_uai "$f")
    fi
    cls="${cn%%$'\t'*}"
    nv="${cn#*$'\t'}"
    copy=no
    case "$cls" in
      boolean|domain1) copy=yes ;;
    esac
    if [ "$copy" = yes ] && [ "$dry_run" = 0 ]; then
      mkdir -p "$stage/$coll/$(dirname "$rel")"
      cp "$f" "$stage/$coll/$rel"
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$coll/$rel" "$coll" "$fam" "$ext" "$nv" "$cls" "$copy" \
      "$(stat -c %s "$f" 2>/dev/null)" >> "$manifest"
    [ $((i % 200)) -eq 0 ] && say "  $i / ${#files[@]}"
  done
}

scan_collection "$evalgm_root" evalgm wcsp
scan_collection "$uai_root" uai uai

# [SUMMARY] files per class and collection, copied files per family
say ""
say "class x collection"
awk -F'\t' 'NR > 1 { c[$2 " " $6]++; t[$2]++ }
  END { for (k in c) { split(k, p, " "); printf "  %-8s %-11s %6d\n", p[1], p[2], c[k] }
        for (k in t) printf "  %-8s TOTAL       %6d\n", k, t[k] }' "$manifest" | sort | tee -a "$log"
say ""
say "copied, per family"
awk -F'\t' 'NR > 1 && $7 == "yes" { c[$2 "/" $3]++ } END { for (k in c) printf "  %-32s %5d\n", k, c[k] }' "$manifest" | sort | tee -a "$log"

if [ "$dry_run" = 1 ]; then
  say ""
  say "dry run: only $manifest was written, the copied column shows what would be copied"
  say "log $log"
  exit 0
fi

# [SWAP] keep the old copy under a new name, then put the new copy in its place
cp "$manifest" "$stage/manifest.tsv"
if [ -d "$dest" ]; then
  bak="${dest}.old-$(date +%F)"
  n=1
  while [ -e "$bak" ]; do
    bak="${dest}.old-$(date +%F)-$n"
    n=$((n + 1))
  done
  mv "$dest" "$bak"
  say ""
  say "old copy moved to $bak"
fi
mv "$stage" "$dest"
say "new copy is at $dest"
say "manifest $manifest  log $log"

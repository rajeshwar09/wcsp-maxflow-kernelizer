#!/usr/bin/env bash
#
# stage_artifact.sh -- build a CLEAN local copy of the published benchmark from the mounted drive, with a manifest file that records what every file is and why it was copied or left behind
#
# File working:
#   1. finds the evalgm/ and uai/ trees under --source
#   2. reads the header of every .wcsp and .uai and classifies it:
#        boolean    every variable has domain size 2      -> COPIED (usable)
#        domain1    domains are only 1 and 2              -> COPIED (recoverable via wcsp_collapse)
#        multi      some variable has domain size >= 3    -> left on the drive (no CCG method can express it)
#        unreadable header could not be parsed            -> left on the drive
#   3. writes the manifest to data/artifact/manifest.tsv
#   4. copies the keepers into a staging folder, preserving relative paths
#   5. swaps: the old --dest is RENAMED to <dest>.old-<date> (never deleted), the staging folder becomes the new --dest
#
#
set -uo pipefail
. "$(dirname "$0")/lib/common.sh"

SRC=""
DEST=~/mtp/wcsp-maxflow/artifact
DRYRUN=0

usage() {
cat <<'USAGE'
Usage: ./scripts/stage_artifact.sh --source <mounted-drive-dir> [options]

Options:
  --source DIR    where the drive is mounted (REQUIRED); the script finds the evalgm/ and uai/ folders anywhere up to 4 levels below it
  --dest DIR      where the local copy lives (default: ~/mtp/wcsp-maxflow/artifact)
  --dry-run       classify and write the manifest only; copy nothing, swap nothing
  -h, --help

Example:
  ./scripts/stage_artifact.sh --source /run/media/raje/<DRIVE>/experiments --dry-run
  ./scripts/stage_artifact.sh --source /run/media/raje/<DRIVE>/experiments
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    --source)  SRC="$2"; shift 2 ;;
    --dest)    DEST="$2"; shift 2 ;;
    --dry-run) DRYRUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *)         echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done
[ -n "$SRC" ] || { echo "error: --source is required" >&2; usage; exit 2; }
[ -d "$SRC" ] || { echo "error: source '$SRC' is not a directory (drive mounted?)" >&2; exit 2; }

mf_init "stage_artifact"
mkdir -p data/artifact

# ------------------------------------------------- find the collection roots

EVALGM_ROOT="$(find "$SRC" -maxdepth 4 -type d -name evalgm 2>/dev/null | head -1)"
UAI_ROOT="$(find "$SRC" -maxdepth 4 -type d \( -name uai -o -name uai-instances \) 2>/dev/null | head -1)"
say "source          : $SRC"
say "evalgm found at : ${EVALGM_ROOT:-NOT FOUND}"
say "uai found at    : ${UAI_ROOT:-NOT FOUND}"
say "dest            : $DEST"
[ -z "$EVALGM_ROOT" ] && [ -z "$UAI_ROOT" ] && { say "nothing to stage."; exit 2; }

MANIFEST="data/artifact/manifest.tsv"
printf 'relpath\tcollection\tfamily\tformat\tnvars\tclass\tcopied\tbytes\n' > "$MANIFEST"
legend_for "$MANIFEST" <<'EOF'
relpath     path of the file relative to the new artifact folder
collection  evalgm or uai
family      parent folder of the instance (e.g. Auction, DBN, MMAP)
format      wcsp (DIMACS) or uai
nvars       number of variables declared in the header
class       boolean    every domain is 2            -> usable directly
            domain1    domains are only 1 and 2     -> usable after wcsp_collapse
            multi      some domain is >= 3          -> unusable for ANY CCG method
            unreadable the header did not parse
copied      yes = present in the local artifact copy, no = left on the drive
bytes       file size in bytes
EOF

# --------------------------------------------------------------- classifiers

# prints "class<TAB>nvars"
classify_wcsp() {
  awk 'NR==1 { nv=$2; if (nv+0<=0) { print "unreadable\tNA"; exit } next }
       NR==2 { cls="boolean"
               for (i=1; i<=NF; i++) {
                 if ($i+0>=3) { cls="multi"; break }
                 if ($i+0==1) cls="domain1"
                 else if ($i+0!=2) { cls="unreadable"; break }
               }
               print cls "\t" nv; exit }
       END   { if (NR<2) print "unreadable\tNA" }' "$1"
}

classify_uai() {
  awk 'NR==2 { nv=$1+0; if (nv<=0) { print "unreadable\tNA"; exit } }
       NR==3 { cls="boolean"
               for (i=1; i<=NF; i++) {
                 if ($i+0>=3) { cls="multi"; break }
                 if ($i+0==1) cls="domain1"
                 else if ($i+0!=2) { cls="unreadable"; break }
               }
               print cls "\t" nv; exit }
       END   { if (NR<3) print "unreadable\tNA" }' "$1"
}

# ------------------------------------------------------------------ stage it

STAGE="${DEST}.staging"
rm -rf "$STAGE"
[ "$DRYRUN" = "0" ] && mkdir -p "$STAGE"

scan_collection() {
  local root="$1" coll="$2" pattern="$3"
  [ -n "$root" ] || return 0
  local total copied=0 i=0
  total=$(find "$root" -name "$pattern" -type f | wc -l)
  say ""
  say "--- $coll: $total files ---"
  find "$root" -name "$pattern" -type f | sort | while IFS= read -r f; do
    i=$((i+1))
    local rel="${f#"$root"/}"
    local fam; fam="$(basename "$(dirname "$f")")"
    local fmt="${pattern#\*.}"
    local cn cls nv
    if [ "$fmt" = "wcsp" ]; then cn="$(classify_wcsp "$f")"; else cn="$(classify_uai "$f")"; fi
    cls="${cn%%	*}"; nv="${cn#*	}"
    local copy=no
    case "$cls" in boolean|domain1) copy=yes ;; esac
    if [ "$copy" = "yes" ] && [ "$DRYRUN" = "0" ]; then
      mkdir -p "$STAGE/$coll/$(dirname "$rel")"
      cp "$f" "$STAGE/$coll/$rel"
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$coll/$rel" "$coll" "$fam" "$fmt" "$nv" "$cls" "$copy" \
      "$(stat -c %s "$f" 2>/dev/null)" >> "$MANIFEST"
    [ $((i % 200)) -eq 0 ] && say "  ...$i/$total"
  done
}

scan_collection "$EVALGM_ROOT" evalgm '*.wcsp'
scan_collection "$UAI_ROOT"    uai    '*.uai'

# ------------------------------------------------------------------ summary

say ""
say "=== manifest summary (class x collection) ==="
awk -F'\t' 'NR>1 {c[$2" "$6]++; t[$2]++}
  END{for(k in c) printf "  %-8s %-11s %6d\n", substr(k,1,index(k," ")-1), substr(k,index(k," ")+1), c[k]
      for(k in t) printf "  %-8s TOTAL       %6d\n", k, t[k]}' "$MANIFEST" | sort | tee -a "$RUNLOG"
say ""
say "=== copied, per family ==="
awk -F'\t' 'NR>1 && $7=="yes" {c[$2"/"$3]++} END{for(k in c) printf "  %-32s %5d\n", k, c[k]}' "$MANIFEST" | sort | tee -a "$RUNLOG"

if [ "$DRYRUN" = "1" ]; then
  say ""
  say "dry run: manifest written to $MANIFEST; the copied column shows the decision that WOULD be taken"
  exit 0
fi

# ---------------------------------------------------------------- the swap

cp "$MANIFEST" "$STAGE/manifest.tsv"
if [ -d "$DEST" ]; then
  BAK="${DEST}.old-$(date +%Y-%m-%d)"
  n=1; while [ -e "$BAK" ]; do BAK="${DEST}.old-$(date +%Y-%m-%d)-$n"; n=$((n+1)); done
  mv "$DEST" "$BAK"
  say ""
  say "previous artifact folder moved to: $BAK"
fi
mv "$STAGE" "$DEST"
say "new artifact copy is live at: $DEST"
say ""
say "done. manifest: $MANIFEST"
#!/usr/bin/env bash
# Usage: prs-since-release.sh <previous production tag> <target commit on dev>
#
# Print one "* <title> by @<author> in <url>" line per PR merged into
# dev after <previous production tag> shipped, up to <target>.
#
# Releases reach main as one squash commit, so a production tag cannot
# reach dev's per-PR history: GitHub's generate-notes anchored at it
# lists every PR ever merged (#139), and so does anything that walks
# from the release branch, which is main plus that squash (#202). The
# point to walk from is the dev commit the release was cut from, found
# in one of two ways:
#
#   1. The release branch's tree is dev's tree at the cut (main is a
#      subset of dev, so squashing dev onto main reproduces it). The
#      newest dev commit with that tree is the cut, even if dev moved
#      on while the release PR was open.
#   2. Failing that, the `-s ours` merge auto-tag-release-pr.yml makes
#      after each release: its first parent is dev as of the sync,
#      which is the cut unless dev moved during the release PR.
#
# First-parent history from there to <target> is exactly the PRs that
# are new. Exits 3 with no output when neither point is found, so
# callers can fall back.
set -euo pipefail

prev_tag=$1
target=$2
repo=${GITHUB_REPOSITORY:?GITHUB_REPOSITORY must name owner/repo}

# Release branches persist after merge; a tag checkout does not have
# them.
git fetch --quiet origin 'refs/heads/release/*:refs/remotes/origin/release/*' || true

# awk reads to the end rather than exiting at the match: an early exit
# kills git log with SIGPIPE, which pipefail turns into a failure.
cut=""
release_ref="refs/remotes/origin/release/$prev_tag"
if git rev-parse --quiet --verify "$release_ref" >/dev/null; then
  release_tree=$(git rev-parse "$release_ref^{tree}")
  cut=$(git log --first-parent --format='%H %T' "$target" \
    | awk -v t="$release_tree" '!found && $2 == t { print $1; found = 1 }')
fi
if [[ -z "$cut" ]]; then
  tag_commit=$(git rev-list -n1 "$prev_tag")
  cut=$(git log --first-parent --merges --format='%H %P' "$target" \
    | awk -v t="$tag_commit" '!found { for (i = 3; i <= NF; i++) if ($i == t) { print $1; found = 1 } }')
fi
if [[ -z "$cut" ]]; then
  echo "Cannot find where $prev_tag was cut on the first-parent history of $target" >&2
  exit 3
fi
echo "Listing PRs in $cut..$target (since $prev_tag)" >&2

for n in $(git log --first-parent --format='%s' "$cut..$target" \
    | grep -oE '\(#[0-9]+\)$' | tr -d '(#)' | sort -n || true); do
  gh pr view "$n" --repo "$repo" --json number,title,author --jq \
    '"* \(.title) by @\(.author.login) in https://github.com/'"$repo"'/pull/\(.number)"'
done

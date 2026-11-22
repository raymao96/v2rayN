#!/usr/bin/env bash
set -euo pipefail

tag="$1"
target="$2"
ref="refs/tags/$tag"

git check-ref-format "$ref"
git cat-file -e "${target}^{commit}"
current="$(git ls-remote --exit-code --refs origin "$ref")"
current="${current%%[[:space:]]*}"

if [[ "$current" == "$target" ]]; then
  echo "$tag already points to $target"
  exit 0
fi

git push --force-with-lease="$ref:$current" origin "$target:$ref"

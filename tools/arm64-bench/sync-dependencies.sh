#!/usr/bin/env bash

sync_bench_dependencies() {
  dependency_sha=$(git ls-files -z --cached --others --exclude-standard -- 'vendor/*.wrap' vendor/packagefiles |
    xargs -0 shasum -a 256 | shasum -a 256 | awk '{print $1}')
  local stamp=vendor/.arm64-bench-inputs.sha256
  local previous_sha
  previous_sha=$(cat "$stamp" 2>/dev/null || true)
  if [[ "$previous_sha" != "$dependency_sha" ]]; then
    echo "Dependency inputs changed or unverified; refreshing generated vendor sources"
    rm -f "$stamp"
    git clean -ffdx -e /vendor/packagecache/ -e /vendor/packagefiles/ \
      -e /vendor/node-compat/ -e '*.wrap' -- vendor/
  fi
  meson subprojects download
  printf '%s\n' "$dependency_sha" > "$stamp"
}

#!/bin/sh

set -eu

script_dir=$(
  CDPATH= cd -- "$(dirname -- "$0")" &&
  pwd
)

repository_root=$(
  git -C "${script_dir}/.." rev-parse --show-toplevel
)

submodule_path="${script_dir}/pico-ws-server"
patch_path="${script_dir}/pico-ws-server.patch"

if [ ! -f "${patch_path}" ]; then
  echo "Patch not found: ${patch_path}" >&2
  exit 1
fi

if [ "${1:-}" = "--revert" ]; then
  if git -C "${submodule_path}" apply -R --check "${patch_path}"; then
    git -C "${submodule_path}" apply -R "${patch_path}"
    echo "Patch reverted"
  else
    echo "Patch is not applied"
  fi
  exit 0
fi

git -C "${repository_root}" submodule update \
  --init \
  --recursive \
  -- submodules/pico-ws-server

if git -C "${submodule_path}" apply \
  --reverse \
  --check \
  "${patch_path}" > /dev/null 2>&1
then
  echo "pico-ws-server patch is already applied"
  exit 0
fi

if ! git -C "${submodule_path}" apply \
  --check \
  "${patch_path}"
then
  echo "pico-ws-server patch does not apply cleanly." >&2
  echo "The pinned submodule revision may have changed." >&2
  exit 1
fi

git -C "${submodule_path}" apply \
  --whitespace=error-all \
  "${patch_path}"

echo "Applied pico-ws-server USB-ECM patch"

  
  


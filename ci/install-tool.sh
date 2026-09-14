#!/bin/bash
# Install one pinned CI tool for the native Ubuntu architecture. No emulation.
set -euo pipefail

tool="${1:?tool is required}"
destination="${2:?destination is required}"
architecture="$(dpkg --print-architecture)"
case "${architecture}" in
    amd64|arm64) ;;
    *) echo "Unsupported CI tool architecture: ${architecture}" >&2; exit 1 ;;
esac

case "${tool}:${architecture}" in
    uv:amd64)
        repository=astral-sh/uv; version=0.12.1
        asset=uv-x86_64-unknown-linux-gnu.tar.gz
        checksum=90b2f223fb69d19db49e117da601f64978593417988530aa733d456141b4bcbb ;;
    uv:arm64)
        repository=astral-sh/uv; version=0.12.1
        asset=uv-aarch64-unknown-linux-gnu.tar.gz
        checksum=769d373e146692c639b5fbaae33b331c297a32e03d30448772051902df52bbf4 ;;
    actionlint:amd64)
        repository=rhysd/actionlint; version=v1.7.7
        asset=actionlint_1.7.7_linux_amd64.tar.gz
        checksum=023070a287cd8cccd71515fedc843f1985bf96c436b7effaecce67290e7e0757 ;;
    actionlint:arm64)
        repository=rhysd/actionlint; version=v1.7.7
        asset=actionlint_1.7.7_linux_arm64.tar.gz
        checksum=401942f9c24ed71e4fe71b76c7d638f66d8633575c4016efd2977ce7c28317d0 ;;
    gitleaks:amd64)
        repository=gitleaks/gitleaks; version=v8.30.1
        asset=gitleaks_8.30.1_linux_x64.tar.gz
        checksum=551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb ;;
    gitleaks:arm64)
        repository=gitleaks/gitleaks; version=v8.30.1
        asset=gitleaks_8.30.1_linux_arm64.tar.gz
        checksum=e4a487ee7ccd7d3a7f7ec08657610aa3606637dab924210b3aee62570fb4b080 ;;
    trivy:amd64)
        repository=aquasecurity/trivy; version=v0.69.3
        asset=trivy_0.69.3_Linux-64bit.tar.gz
        checksum=1816b632dfe529869c740c0913e36bd1629cb7688bd5634f4a858c1d57c88b75 ;;
    trivy:arm64)
        repository=aquasecurity/trivy; version=v0.69.3
        asset=trivy_0.69.3_Linux-ARM64.tar.gz
        checksum=7e3924a974e912e57b4a99f65ece7931f8079584dae12eb7845024f97087bdfd ;;
    *) echo "Unsupported CI tool: ${tool}" >&2; exit 1 ;;
esac

archive_directory="$(mktemp -d)"
trap 'rm -rf -- "${archive_directory}"' EXIT
archive="${archive_directory}/${asset}"
curl --fail --silent --show-error --location --retry 5 --retry-all-errors \
    --connect-timeout 15 --max-time 180 \
    "https://github.com/${repository}/releases/download/${version}/${asset}" -o "${archive}"
printf '%s  %s\n' "${checksum}" "${archive}" | sha256sum --check
mkdir -p "${destination}"
if [ "${tool}" = uv ]; then
    member_root="${asset%.tar.gz}"
    tar xzf "${archive}" -C "${destination}" --strip-components=1 \
        "${member_root}/uv" "${member_root}/uvx"
else
    tar xzf "${archive}" -C "${destination}" "${tool}"
fi

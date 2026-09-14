#!/usr/bin/env python3
"""Verify CI installer selection and failure order with controlled OS/network tools."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ASSETS = {
    ("uv", "amd64"): (
        "uv-x86_64-unknown-linux-gnu.tar.gz",
        "90b2f223fb69d19db49e117da601f64978593417988530aa733d456141b4bcbb",
    ),
    ("uv", "arm64"): (
        "uv-aarch64-unknown-linux-gnu.tar.gz",
        "769d373e146692c639b5fbaae33b331c297a32e03d30448772051902df52bbf4",
    ),
    ("actionlint", "amd64"): (
        "actionlint_1.7.7_linux_amd64.tar.gz",
        "023070a287cd8cccd71515fedc843f1985bf96c436b7effaecce67290e7e0757",
    ),
    ("actionlint", "arm64"): (
        "actionlint_1.7.7_linux_arm64.tar.gz",
        "401942f9c24ed71e4fe71b76c7d638f66d8633575c4016efd2977ce7c28317d0",
    ),
    ("gitleaks", "amd64"): (
        "gitleaks_8.30.1_linux_x64.tar.gz",
        "551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb",
    ),
    ("gitleaks", "arm64"): (
        "gitleaks_8.30.1_linux_arm64.tar.gz",
        "e4a487ee7ccd7d3a7f7ec08657610aa3606637dab924210b3aee62570fb4b080",
    ),
    ("trivy", "amd64"): (
        "trivy_0.69.3_Linux-64bit.tar.gz",
        "1816b632dfe529869c740c0913e36bd1629cb7688bd5634f4a858c1d57c88b75",
    ),
    ("trivy", "arm64"): (
        "trivy_0.69.3_Linux-ARM64.tar.gz",
        "7e3924a974e912e57b4a99f65ece7931f8079584dae12eb7845024f97087bdfd",
    ),
}
SHIM = r"""
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
data = sys.stdin.read() if name == 'sha256sum' else ''
with open(os.environ['CALLS'], 'a') as output:
    output.write(json.dumps([name, args, data]) + '\n')
if name == 'dpkg':
    print(os.environ['TEST_ARCH'])
if name == 'sha256sum' and os.environ.get('BAD_CHECKSUM') == '1':
    sys.exit(1)
"""


class CIToolContract(unittest.TestCase):
    def run_installer(self, tool: str, arch: str, bad_checksum: bool = False):
        with tempfile.TemporaryDirectory(prefix="nestor-tool-contract-") as directory:
            root = Path(directory)
            binary = root / "bin"
            binary.mkdir()
            for name in ("dpkg", "curl", "sha256sum", "tar"):
                path = binary / name
                path.write_text("#!" + sys.executable + "\n" + SHIM)
                path.chmod(0o700)
            environment = {
                "PATH": str(binary) + os.pathsep + os.environ.get("PATH", ""),
                "HOME": str(root),
                "TMPDIR": str(root),
                "CALLS": str(root / "calls"),
                "TEST_ARCH": arch,
                "BAD_CHECKSUM": "1" if bad_checksum else "0",
            }
            result = subprocess.run(
                [
                    shutil.which("bash") or "/bin/bash",
                    str(ROOT / "ci/install-tool.sh"),
                    tool,
                    str(root / "destination"),
                ],
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=10,
                check=False,
            )
            calls = (
                [json.loads(line) for line in (root / "calls").read_text().splitlines()]
                if (root / "calls").exists()
                else []
            )
            return result.returncode, calls, result.stdout

    def test_exact_release_for_each_supported_native_architecture(self):
        for (tool, arch), (asset, digest) in ASSETS.items():
            with self.subTest(tool=tool, arch=arch):
                code, calls, output = self.run_installer(tool, arch)
                self.assertEqual(code, 0, output)
                download = next(call for call in calls if call[0] == "curl")
                self.assertTrue(
                    any(value.endswith("/" + asset) for value in download[1]), download
                )
                checksum_index = next(
                    i for i, call in enumerate(calls) if call[0] == "sha256sum"
                )
                extract_index = next(
                    i for i, call in enumerate(calls) if call[0] == "tar"
                )
                self.assertIn(digest + "  ", calls[checksum_index][2])
                self.assertLess(checksum_index, extract_index)

    def test_unknown_architecture_fails_before_network_or_extraction(self):
        code, calls, output = self.run_installer("uv", "riscv64")
        self.assertNotEqual(code, 0, output)
        self.assertFalse(any(call[0] in ("curl", "tar") for call in calls), calls)

    def test_checksum_failure_prevents_extraction(self):
        code, calls, output = self.run_installer("uv", "arm64", bad_checksum=True)
        self.assertNotEqual(code, 0, output)
        self.assertTrue(any(call[0] == "sha256sum" for call in calls), calls)
        self.assertFalse(any(call[0] == "tar" for call in calls), calls)


if __name__ == "__main__":
    unittest.main()

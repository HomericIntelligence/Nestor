#!/usr/bin/env python3
"""Exercise the actual CI launcher with private, controlled external tools.

No container, compiler, package installer, scanner or broker is started. The
engine substitute executes the launcher's actual inner Bash blocks; tool exit
codes and call receipts expose orchestration errors without claiming real CI.
"""

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
TOOLS = (
    "conan",
    "cmake",
    "ctest",
    "trivy",
    "gitleaks",
    "actionlint",
    "uv",
    "clang-format",
    "clang-tidy",
    "yamllint",
    "check-jsonschema",
    "python3",
    "curl",
)
SHIM = r"""
import json, os, pathlib, shutil, subprocess, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
root = pathlib.Path(os.environ['FIXTURE_ROOT'])
config = json.loads(os.environ['FIXTURE_CONFIG'])
with (root / 'calls.jsonl').open('a') as output:
    output.write(json.dumps([name, *args]) + '\n')
if name == 'engine':
    if args[0] in ('image', 'images'):
        sys.exit(0)
    if args[0] in ('rm', 'stop', 'start'):
        sys.exit(17 if config.get('fail_removal') else 0)
    if args[0] == 'port':
        print('127.0.0.1:14222' if '4222' in args[-1] else '127.0.0.1:18222')
        sys.exit(0)
    if args[0] == 'run' and '--cidfile' in args:
        pathlib.Path(args[args.index('--cidfile') + 1]).write_text(('b' if '-d' in args else 'c') * 64)
    if args[0] == 'run' and '-d' in args:
        print('b' * 64)
        sys.exit(0)
    index = args.index('nestor-ci:local')
    command = args[index + 1:]
    if command[0] == 'bash':
        command[0] = os.environ['FIXTURE_BASH']
        command[-1] = command[-1].replace('/workspace', str(root))
    sys.exit(subprocess.run(command, cwd=root, check=False).returncode)
if name == config.get('fail_tool'):
    sys.exit(19)
if name == 'cmake' and config.get('remove_release') and '--build' in args:
    shutil.rmtree(root / 'build/release')
if name == 'cmake' and config.get('fail_build') and '--build' in args:
    sys.exit(23)
if name == 'cmake' and config.get('fail_configure') and '--build' not in args:
    sys.exit(24)
if name == 'ctest':
    label = args[args.index('-L') + 1] if '-L' in args else ''
    if label in config.get('empty_labels', []):
        print('No tests were found!!!')
        sys.exit(8 if '--no-tests=error' in args else 0)
    if label in config.get('failed_labels', []):
        print('Controlled labelled test failed')
        sys.exit(8)
sys.exit(0)
"""


class LocalCIContract(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="nestor-ci-contract-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        for relative in (
            "scripts",
            "build/release",
            "bin",
            "src",
            "include",
            ".github/workflows",
            "home",
        ):
            (self.root / relative).mkdir(parents=True, exist_ok=True)
        shutil.copy2(
            ROOT / "scripts/run_ci_local.sh", self.root / "scripts/run_ci_local.sh"
        )
        (self.root / "src/example.cpp").write_text("int main() { return 0; }\n")
        (self.root / ".github/workflows/example.yml").write_text("name: fixture\n")
        for name in ("engine", *TOOLS):
            path = self.root / "bin" / name
            path.write_text("#!" + sys.executable + "\n" + SHIM)
            path.chmod(0o700)

    def run_ci(self, subset: str, **config: object) -> tuple[int, list[list[str]], str]:
        environment = {
            "PATH": str(self.root / "bin") + os.pathsep + os.environ.get("PATH", ""),
            "HOME": str(self.root / "home"),
            "TMPDIR": str(self.root),
            "CONTAINER_ENGINE": str(self.root / "bin/engine"),
            "FIXTURE_ROOT": str(self.root),
            "FIXTURE_CONFIG": json.dumps(config),
            "FIXTURE_BASH": shutil.which("bash") or "/bin/bash",
        }
        result = subprocess.run(
            [
                environment["FIXTURE_BASH"],
                str(self.root / "scripts/run_ci_local.sh"),
                subset,
            ],
            cwd=self.root,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=15,
            check=False,
        )
        calls_path = self.root / "calls.jsonl"
        calls = (
            [json.loads(line) for line in calls_path.read_text().splitlines()]
            if calls_path.exists()
            else []
        )
        return result.returncode, calls, result.stdout

    def test_failed_build_cannot_continue_into_test_execution(self) -> None:
        for failure in ("fail_configure", "fail_build"):
            with self.subTest(failure=failure):
                code, calls, output = self.run_ci("unit", **{failure: True})
                self.assertNotEqual(code, 0, output)
                self.assertFalse(any(call[0] == "ctest" for call in calls), calls)

    def test_existing_cache_does_not_replace_current_source_build(self) -> None:
        (self.root / "build/release/CMakeCache.txt").write_text("old-source-cache\n")
        code, calls, output = self.run_ci("build")
        self.assertEqual(code, 0, output)
        self.assertTrue(any(call[:2] == ["cmake", "--preset"] for call in calls), calls)
        self.assertTrue(any(call[:2] == ["cmake", "--build"] for call in calls), calls)

    def test_empty_required_selection_fails(self) -> None:
        for label in ("unit", "integration", "concurrency"):
            with self.subTest(label=label):
                code, _, output = self.run_ci(label, empty_labels=[label])
                self.assertNotEqual(code, 0, output)

    def test_failed_label_is_not_replaced_with_another_selection(self) -> None:
        for label in ("unit", "integration"):
            with self.subTest(label=label):
                code, calls, output = self.run_ci(label, failed_labels=[label])
                self.assertNotEqual(code, 0, output)
                self.assertFalse(
                    any(call[0] == "ctest" and "-L" not in call for call in calls),
                    calls,
                )

    def test_actual_tool_errors_propagate(self) -> None:
        for subset, tool in (
            ("security", "trivy"),
            ("secrets", "gitleaks"),
            ("actionlint", "actionlint"),
        ):
            with self.subTest(tool=tool):
                code, _, output = self.run_ci(subset, fail_tool=tool)
                self.assertNotEqual(code, 0, output)

    def test_all_executes_declared_local_broker_and_workflow_gates(self) -> None:
        code, calls, output = self.run_ci("all")
        self.assertEqual(code, 0, output)
        self.assertTrue(any(call[0] == "actionlint" for call in calls), calls)
        self.assertTrue(
            any(call[0] == "ctest" and "live-nats" in call for call in calls), calls
        )

    def test_live_broker_ports_are_private_and_assigned_by_the_engine(self) -> None:
        code, calls, output = self.run_ci("nats")
        self.assertEqual(code, 0, output)
        broker = next(
            call for call in calls if call[:2] == ["engine", "run"] and "-d" in call
        )
        self.assertIn("127.0.0.1::4222", broker)
        self.assertIn("127.0.0.1::8222", broker)
        self.assertNotIn("nestor-nats-test-nats-1", broker)
        self.assertTrue(any(call[:2] == ["engine", "port"] for call in calls), calls)

    def test_failed_build_cannot_start_a_broker(self) -> None:
        code, calls, output = self.run_ci("nats", fail_build=True)
        self.assertNotEqual(code, 0, output)
        self.assertFalse(
            any(call[:2] == ["engine", "run"] and "-d" in call for call in calls), calls
        )

    def test_failing_live_tests_remove_the_exact_created_broker(self) -> None:
        code, calls, output = self.run_ci("nats", failed_labels=["live-nats"])
        self.assertNotEqual(code, 0, output)
        self.assertIn(["engine", "rm", "-f", "b" * 64], calls)

    def test_missing_concurrency_directory_cannot_skip_test_execution(self) -> None:
        code, calls, output = self.run_ci("concurrency", remove_release=True)
        self.assertNotEqual(code, 0, output)
        self.assertFalse(any(call[0] == "ctest" for call in calls), calls)

    def test_failed_removal_retains_owned_container_receipts(self) -> None:
        code, calls, output = self.run_ci("nats", fail_removal=True)
        self.assertNotEqual(code, 0, output)
        broker = next(
            call for call in calls if call[:2] == ["engine", "run"] and "-d" in call
        )
        receipt = Path(broker[broker.index("--cidfile") + 1])
        self.assertTrue(receipt.is_file(), output)
        self.assertEqual(receipt.read_text(), "b" * 64)

    def test_existing_scanner_findings_policy_is_preserved(self) -> None:
        code, calls, output = self.run_ci("all")
        self.assertEqual(code, 0, output)
        for tool in ("gitleaks", "trivy"):
            call = next(call for call in calls if call[0] == tool)
            self.assertEqual(call[call.index("--exit-code") + 1], "0")
        self.assertIn(
            "HIGH,CRITICAL", next(call for call in calls if call[0] == "trivy")
        )
        self.assertFalse(any(call[:3] == ["conan", "audit", "scan"] for call in calls))


if __name__ == "__main__":
    unittest.main()

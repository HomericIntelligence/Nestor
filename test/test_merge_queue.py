#!/usr/bin/env python3
"""Behavioral regression tests for staged GitHub merge-queue readiness."""

from __future__ import annotations

import json
import unittest
from pathlib import Path
from typing import Any

import yaml


REPO_ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"
POLICY_PATH = REPO_ROOT / "configs" / "github" / "merge-queue-policy.json"
RUNBOOK_PATH = REPO_ROOT / "docs" / "governance" / "merge-queue.md"

REQUIRED_WORKFLOW_TRIGGERS = {
    "_required.yml": {
        "push": {"branches": ["main"]},
        "pull_request": {"branches": ["main"]},
        "merge_group": {"types": ["checks_requested"]},
    },
    "build-test.yml": {
        "push": {"branches": ["main"]},
        "pull_request": {"branches": ["main"]},
        "merge_group": {"types": ["checks_requested"]},
    },
    "code-coverage.yml": {
        "push": {"branches": ["main"]},
        "pull_request": {"branches": ["main"]},
        "merge_group": {"types": ["checks_requested"]},
    },
    "docker-publish.yml": {
        "push": {"branches": ["main"], "tags": ["v*.*.*"]},
        "pull_request": {"branches": ["main"]},
        "merge_group": {"types": ["checks_requested"]},
    },
    "static-analysis.yml": {
        "push": {"branches": ["main"]},
        "pull_request": {"branches": ["main"]},
        "merge_group": {"types": ["checks_requested"]},
    },
}

EXPECTED_REQUIRED_CHECKS = [
    {
        "context": "All Build/Test Checks",
        "workflow": "build-test.yml",
        "authority": "homeric-main-extras",
    },
    {
        "context": "All Coverage Checks",
        "workflow": "code-coverage.yml",
        "authority": "homeric-main-extras",
    },
    {
        "context": "All Static Analysis Checks",
        "workflow": "static-analysis.yml",
        "authority": "homeric-main-extras",
    },
    {
        "context": "branch-protection-drift",
        "workflow": "_required.yml",
        "authority": "homeric-main-extras",
    },
    {
        "context": "build",
        "workflow": "_required.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "deps/version-sync",
        "workflow": "_required.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "install",
        "workflow": "docker-publish.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "integration-tests",
        "workflow": "_required.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "lint",
        "workflow": "_required.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "package",
        "workflow": "docker-publish.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "release",
        "workflow": "docker-publish.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "schema-validation",
        "workflow": "_required.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "security/dependency-scan",
        "workflow": "_required.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "security/secrets-scan",
        "workflow": "_required.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "test",
        "workflow": "_required.yml",
        "authority": "homeric-main-baseline",
    },
    {
        "context": "unit-tests",
        "workflow": "_required.yml",
        "authority": "homeric-main-baseline",
    },
]

EXPECTED_QUEUE_RULE = {
    "type": "merge_queue",
    "parameters": {
        "check_response_timeout_minutes": 60,
        "grouping_strategy": "ALLGREEN",
        "max_entries_to_build": 10,
        "max_entries_to_merge": 5,
        "merge_method": "SQUASH",
        "min_entries_to_merge": 1,
        "min_entries_to_merge_wait_minutes": 5,
    },
}


def load_workflow(filename: str) -> dict[str, Any]:
    data = yaml.safe_load((WORKFLOWS_DIR / filename).read_text())
    if not isinstance(data, dict):
        raise AssertionError(f"{filename} must contain a workflow mapping")
    return data


def on_block(workflow: dict[str, Any]) -> dict[str, Any]:
    """Return the trigger block despite PyYAML's YAML 1.1 `on` coercion."""
    value = workflow.get(True, workflow.get("on"))
    if not isinstance(value, dict):
        raise AssertionError("workflow `on` block must be a mapping")
    return value


def load_policy() -> dict[str, Any]:
    data = json.loads(POLICY_PATH.read_text())
    if not isinstance(data, dict):
        raise AssertionError("merge-queue policy must be a JSON object")
    return data


def named_step(workflow: dict[str, Any], job_id: str, step_name: str) -> dict[str, Any]:
    steps = workflow["jobs"][job_id]["steps"]
    return next(step for step in steps if step.get("name") == step_name)


class MergeQueueReadinessTests(unittest.TestCase):
    def test_policy_pins_repository_target_and_activation_ruleset(self) -> None:
        policy = load_policy()
        self.assertEqual(policy["repository"], "HomericIntelligence/Nestor")
        self.assertEqual(policy["target_branch"], "main")
        self.assertEqual(policy["activation_ruleset"], "homeric-main-baseline")

    def test_policy_pins_exact_required_check_mapping(self) -> None:
        self.assertEqual(load_policy()["required_checks"], EXPECTED_REQUIRED_CHECKS)

    def test_policy_pins_exact_queue_rule(self) -> None:
        self.assertEqual(load_policy()["merge_queue_rule"], EXPECTED_QUEUE_RULE)

    def test_every_required_workflow_has_exact_trigger_contract(self) -> None:
        for filename, expected in REQUIRED_WORKFLOW_TRIGGERS.items():
            with self.subTest(workflow=filename):
                self.assertEqual(on_block(load_workflow(filename)), expected)

    def test_codeql_schedule_and_security_permissions_remain_unchanged(self) -> None:
        workflow = load_workflow("codeql.yml")
        self.assertEqual(
            on_block(workflow),
            {
                "push": {"branches": ["main"]},
                "pull_request": {"branches": ["main"]},
                "schedule": [{"cron": "23 5 * * 1"}],
            },
        )
        self.assertEqual(
            workflow["permissions"], {"contents": "read", "security-events": "write"}
        )

    def test_required_contexts_are_emitted_once_by_declared_workflows(self) -> None:
        policy = load_policy()
        expected = [item["context"] for item in policy["required_checks"]]
        emitted: list[str] = []

        for filename in REQUIRED_WORKFLOW_TRIGGERS:
            jobs = load_workflow(filename)["jobs"]
            for job_id, job in jobs.items():
                emitted.append(job.get("name", job_id))

        policy_names = [name for name in emitted if name in expected]
        self.assertEqual(sorted(policy_names), expected)
        self.assertEqual(len(policy_names), len(set(policy_names)))

    def test_merge_group_build_never_publishes_or_logs_into_ghcr(self) -> None:
        workflow = load_workflow("docker-publish.yml")
        login = named_step(workflow, "build", "Log in to GHCR")
        publish = named_step(workflow, "build", "Build and (conditionally) push")

        self.assertEqual(login["if"], "github.event_name == 'push'")
        self.assertEqual(publish["with"]["push"], "${{ github.event_name == 'push' }}")
        self.assertEqual(workflow["permissions"], {"contents": "read", "packages": "write"})

    def test_required_schema_gate_executes_this_regression_suite(self) -> None:
        workflow = load_workflow("_required.yml")
        schema_step = named_step(workflow, "schema-validation", "Validate workflow schemas")
        step = named_step(workflow, "schema-validation", "Test merge-queue readiness")
        self.assertIn("pip install check-jsonschema PyYAML", schema_step["run"])
        self.assertEqual(step["run"], "python3 test/test_merge_queue.py")

    def test_runbook_keeps_activation_and_smoke_post_merge(self) -> None:
        runbook = RUNBOOK_PATH.read_text()
        for marker in (
            "human workflow review",
            "Post-merge activation",
            "merge_queue_rule",
            "required_checks",
            "merge_group",
            "Rollback",
            "issue #128",
        ):
            with self.subTest(marker=marker):
                self.assertIn(marker, runbook)


if __name__ == "__main__":
    unittest.main(verbosity=2)

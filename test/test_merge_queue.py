#!/usr/bin/env python3
"""Behavioral regression tests for staged GitHub merge-queue readiness."""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path
from typing import Any

import yaml


REPO_ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"
POLICY_PATH = REPO_ROOT / "configs" / "github" / "merge-queue-policy.json"
LOCK_PATH = REPO_ROOT / "uv.lock"
BRANCH_PROTECTION_PATH = REPO_ROOT / ".github" / "branch-protection" / "main.json"
DRIFT_SCRIPT_PATH = REPO_ROOT / "scripts" / "verify-branch-protection.sh"
GOVERNANCE_DOCS = (
    REPO_ROOT / "docs" / "governance" / "branch-protection.md",
    REPO_ROOT / "docs" / "governance" / "merge-queue.md",
)

# Every live required-context producer must execute the same real closure for
# pull requests and synthetic merge-group commits.
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

EXPECTED_CONCURRENCY_GROUP = (
    "${{ github.workflow }}-${{ github.event_name }}-"
    "${{ github.event.pull_request.number || github.sha }}"
)

ALLOWED_REQUIRED_JOB_CONDITIONS = {
    "test": "always()",
    "All Build/Test Checks": "always()",
    "All Coverage Checks": "always()",
    "All Static Analysis Checks": "always()",
}

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

EXPECTED_RULESETS = {
    "homeric-main-baseline": {
        "target": "branch",
        "enforcement": "active",
        "source_type": "Repository",
        "source": "HomericIntelligence/Nestor",
        "conditions": {
            "ref_name": {
                "exclude": [],
                "include": ["refs/heads/main"],
            }
        },
    },
    "homeric-main-extras": {
        "target": "branch",
        "enforcement": "active",
        "source_type": "Repository",
        "source": "HomericIntelligence/Nestor",
        "conditions": {
            "ref_name": {
                "exclude": [],
                "include": ["refs/heads/main"],
            }
        },
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


def workflow_job_names(filename: str) -> list[str]:
    jobs = load_workflow(filename)["jobs"]
    return [job.get("name", job_id) for job_id, job in jobs.items()]


def workflow_paths() -> list[Path]:
    """Return every workflow file GitHub recognizes, in stable order."""
    return sorted(
        path for path in WORKFLOWS_DIR.iterdir() if path.suffix in {".yml", ".yaml"}
    )


def render_concurrency_group(
    workflow: str,
    event_name: str,
    *,
    pull_request_number: int | None = None,
    sha: str,
) -> str:
    """Model the exact workflow concurrency expression for behavior tests."""
    event_identity: int | str = pull_request_number or sha
    return f"{workflow}-{event_name}-{event_identity}"


def policy_contexts_by_authority() -> dict[str, list[str]]:
    split: dict[str, list[str]] = {}
    for required_check in load_policy()["required_checks"]:
        split.setdefault(required_check["authority"], []).append(
            required_check["context"]
        )
    return {authority: sorted(contexts) for authority, contexts in split.items()}


def run_drift_check(
    split: dict[str, list[str]],
    ruleset_overrides: dict[str, dict[str, Any]] | None = None,
) -> subprocess.CompletedProcess[str]:
    policy = load_policy()
    repository = policy["repository"]
    target_branch = policy["target_branch"]
    authorities = sorted(policy_contexts_by_authority())
    ids = {authority: index + 100 for index, authority in enumerate(authorities)}
    ruleset_overrides = ruleset_overrides or {}
    responses: dict[str, Any] = {
        f"repos/{repository}/rules/branches/{target_branch}": [
            {
                "type": "pull_request",
                "parameters": {
                    "required_approving_review_count": 0,
                    "required_review_thread_resolution": True,
                },
            },
            *[
                {
                    "type": "required_status_checks",
                    "parameters": {
                        "required_status_checks": [
                            {"context": context} for context in contexts
                        ]
                    },
                }
                for contexts in split.values()
            ],
        ],
        f"repos/{repository}/rulesets?includes_parents=true": [],
    }
    for authority, contexts in split.items():
        contract = deepcopy(EXPECTED_RULESETS[authority])
        contract.update(deepcopy(ruleset_overrides.get(authority, {})))
        responses[f"repos/{repository}/rulesets?includes_parents=true"].append(
            {"id": ids[authority], "name": authority, **contract}
        )
        responses[f"repos/{repository}/rulesets/{ids[authority]}"] = {
            "id": ids[authority],
            "name": authority,
            **contract,
            "rules": [
                {
                    "type": "required_status_checks",
                    "parameters": {
                        "required_status_checks": [
                            {"context": context} for context in contexts
                        ]
                    },
                }
            ],
        }

    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        responses_path = temp / "responses.json"
        responses_path.write_text(json.dumps(responses))
        gh_path = temp / "gh"
        gh_path.write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "if len(sys.argv) != 3 or sys.argv[1] != 'api':\n"
            "    raise SystemExit(f'unexpected gh invocation: {sys.argv[1:]}')\n"
            "responses = json.load(open(os.environ['MOCK_GH_RESPONSES']))\n"
            "key = sys.argv[2]\n"
            "if key not in responses:\n"
            "    raise SystemExit(f'unexpected endpoint: {key}')\n"
            "print(json.dumps(responses[key]))\n"
        )
        gh_path.chmod(0o755)
        env = os.environ.copy()
        env["PATH"] = f"{temp}:{env['PATH']}"
        env["MOCK_GH_RESPONSES"] = str(responses_path)
        return subprocess.run(
            ["bash", str(DRIFT_SCRIPT_PATH)],
            cwd=REPO_ROOT,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )


class MergeQueueReadinessTests(unittest.TestCase):
    def test_policy_pins_repository_target_and_activation_ruleset(self) -> None:
        policy = load_policy()
        self.assertEqual(policy["repository"], "HomericIntelligence/Nestor")
        self.assertEqual(policy["target_branch"], "main")
        self.assertEqual(policy["activation_ruleset"], "homeric-main-baseline")
        self.assertIn(policy["activation_ruleset"], EXPECTED_RULESETS)

    def test_policy_pins_each_managed_ruleset_contract(self) -> None:
        policy = load_policy()
        self.assertEqual(
            {
                item["name"]: {
                    key: value for key, value in item.items() if key != "name"
                }
                for item in policy["rulesets"]
            },
            EXPECTED_RULESETS,
        )

    def test_policy_contexts_exist_once_in_their_declared_workflows(self) -> None:
        required_checks = load_policy()["required_checks"]
        contexts = [item["context"] for item in required_checks]
        self.assertEqual(
            len(contexts), len(set(contexts)), "policy contexts must be unique"
        )

        workflows = {path.name for path in workflow_paths()}
        emitted_by_workflow = {
            workflow: workflow_job_names(workflow) for workflow in workflows
        }
        for item in required_checks:
            with self.subTest(context=item["context"], workflow=item["workflow"]):
                self.assertIn(item["context"], emitted_by_workflow[item["workflow"]])
                self.assertEqual(
                    emitted_by_workflow[item["workflow"]].count(item["context"]),
                    1,
                )
                owners = [
                    workflow
                    for workflow, emitted in emitted_by_workflow.items()
                    if item["context"] in emitted
                ]
                self.assertEqual(owners, [item["workflow"]])

    def test_legacy_branch_protection_contexts_derive_from_policy(self) -> None:
        branch_protection = json.loads(BRANCH_PROTECTION_PATH.read_text())
        self.assertEqual(
            sorted(branch_protection["required_status_checks"]["contexts"]),
            policy_contexts_by_authority()["homeric-main-extras"],
        )

    def test_policy_pins_exact_queue_rule(self) -> None:
        self.assertEqual(load_policy()["merge_queue_rule"], EXPECTED_QUEUE_RULE)

    def test_policy_transform_appends_only_the_approved_queue_rule(self) -> None:
        snapshot = {
            "name": "homeric-main-baseline",
            "target": "branch",
            "enforcement": "active",
            "rules": [{"type": "deletion"}, {"type": "required_status_checks"}],
        }
        result = subprocess.run(
            [
                "jq",
                "--slurpfile",
                "policy",
                str(POLICY_PATH),
                ".rules += [$policy[0].merge_queue_rule]",
            ],
            input=json.dumps(snapshot),
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        transformed = json.loads(result.stdout)
        self.assertEqual(transformed["rules"][:-1], snapshot["rules"])
        self.assertEqual(transformed["rules"][-1], load_policy()["merge_queue_rule"])
        self.assertEqual(
            {key: value for key, value in transformed.items() if key != "rules"},
            {key: value for key, value in snapshot.items() if key != "rules"},
        )

    def test_every_required_workflow_has_exact_trigger_contract(self) -> None:
        policy_workflows = {
            item["workflow"] for item in load_policy()["required_checks"]
        }
        self.assertEqual(set(REQUIRED_WORKFLOW_TRIGGERS), policy_workflows)
        for filename, expected in REQUIRED_WORKFLOW_TRIGGERS.items():
            with self.subTest(workflow=filename):
                self.assertEqual(on_block(load_workflow(filename)), expected)

    def test_required_contexts_have_equal_pr_and_merge_group_reachability(self) -> None:
        required_checks = load_policy()["required_checks"]
        contexts_by_event: dict[str, set[str]] = {
            "pull_request": set(),
            "merge_group": set(),
        }

        for item in required_checks:
            workflow = load_workflow(item["workflow"])
            job = next(
                candidate
                for candidate in workflow["jobs"].values()
                if candidate.get("name") == item["context"]
            )
            condition = str(job.get("if", ""))
            self.assertEqual(
                condition,
                ALLOWED_REQUIRED_JOB_CONDITIONS.get(item["context"], ""),
            )
            for event_name in contexts_by_event:
                self.assertIn(event_name, on_block(workflow))
                contexts_by_event[event_name].add(item["context"])

        expected = {item["context"] for item in required_checks}
        self.assertEqual(contexts_by_event["pull_request"], expected)
        self.assertEqual(contexts_by_event["merge_group"], expected)

    def test_required_workflow_concurrency_is_event_and_identity_safe(self) -> None:
        for filename in REQUIRED_WORKFLOW_TRIGGERS:
            with self.subTest(workflow=filename):
                concurrency = load_workflow(filename).get("concurrency")
                self.assertIsInstance(concurrency, dict)
                self.assertEqual(concurrency["group"], EXPECTED_CONCURRENCY_GROUP)
                self.assertTrue(concurrency["cancel-in-progress"])

        first = render_concurrency_group(
            "Required Checks",
            "pull_request",
            pull_request_number=101,
            sha="same-head-sha",
        )
        second = render_concurrency_group(
            "Required Checks",
            "pull_request",
            pull_request_number=202,
            sha="same-head-sha",
        )
        stale = render_concurrency_group(
            "Required Checks",
            "pull_request",
            pull_request_number=101,
            sha="new-head-sha",
        )
        queued = render_concurrency_group(
            "Required Checks",
            "merge_group",
            sha="synthetic-sha",
        )
        self.assertNotEqual(first, second, "unrelated fork PRs must not cancel")
        self.assertEqual(first, stale, "new runs of one PR should cancel stale runs")
        self.assertNotEqual(first, queued, "PR and merge-group runs must not cancel")

    def test_smoke_only_merge_queue_carriers_are_absent(self) -> None:
        for path in workflow_paths():
            with self.subTest(workflow=path.name):
                text = path.read_text()
                self.assertNotIn("merge-queue-smoke", text)
                self.assertNotEqual(path.name, "merge-queue-smoke.yml")
                self.assertNotEqual(path.name, "merge-queue-smoke.yaml")

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

    def test_drift_preflight_accepts_exact_policy_split(self) -> None:
        result = run_drift_check(policy_contexts_by_authority())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_drift_preflight_rejects_context_in_wrong_ruleset(self) -> None:
        split = policy_contexts_by_authority()
        authorities = sorted(split)
        moved = split[authorities[0]].pop()
        split[authorities[1]].append(moved)
        result = run_drift_check(split)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_drift_preflight_rejects_missing_context(self) -> None:
        split = policy_contexts_by_authority()
        split[sorted(split)[0]].pop()
        result = run_drift_check(split)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_drift_preflight_rejects_ruleset_target_drift(self) -> None:
        result = run_drift_check(
            policy_contexts_by_authority(),
            {"homeric-main-baseline": {"target": "tag"}},
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_drift_preflight_rejects_ruleset_enforcement_drift(self) -> None:
        result = run_drift_check(
            policy_contexts_by_authority(),
            {"homeric-main-baseline": {"enforcement": "disabled"}},
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_drift_preflight_rejects_ruleset_source_type_drift(self) -> None:
        result = run_drift_check(
            policy_contexts_by_authority(),
            {"homeric-main-baseline": {"source_type": "Organization"}},
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_drift_preflight_rejects_ruleset_source_drift(self) -> None:
        result = run_drift_check(
            policy_contexts_by_authority(),
            {"homeric-main-baseline": {"source": "HomericIntelligence/Other"}},
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_drift_preflight_rejects_ruleset_branch_condition_drift(self) -> None:
        result = run_drift_check(
            policy_contexts_by_authority(),
            {
                "homeric-main-baseline": {
                    "conditions": {
                        "ref_name": {
                            "exclude": [],
                            "include": ["refs/heads/develop"],
                        }
                    }
                }
            },
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_merge_group_build_never_publishes_or_logs_into_ghcr(self) -> None:
        workflow = load_workflow("docker-publish.yml")
        login = named_step(workflow, "build", "Log in to GHCR")
        publish = named_step(workflow, "build", "Build and (conditionally) push")

        self.assertEqual(login["if"], "github.event_name == 'push'")
        self.assertEqual(publish["with"]["push"], "${{ github.event_name == 'push' }}")
        self.assertEqual(
            workflow["permissions"], {"contents": "read", "packages": "write"}
        )

    def test_required_schema_gate_executes_this_regression_suite(self) -> None:
        workflow = load_workflow("_required.yml")
        schema_step = named_step(
            workflow, "schema-validation", "Validate workflow schemas"
        )
        step = named_step(workflow, "schema-validation", "Test merge-queue readiness")
        # uv.lock records each dependency as a `[[package]]` TOML block; read the
        # PyYAML block's `version` field (the canonical uv pin) rather than parsing
        # wheel-URL filenames, which vary between sdist/wheel entries.
        lock_text = LOCK_PATH.read_text()
        pyyaml_block = re.search(
            r'\[\[package\]\]\nname = "pyyaml"\nversion = "([^"]+)"',
            lock_text,
        )
        locked_versions = {pyyaml_block.group(1)} if pyyaml_block else set()
        self.assertEqual(len(locked_versions), 1)
        locked_version = locked_versions.pop()
        self.assertIn(f"PyYAML=={locked_version}", schema_step["run"])
        # Podman-first: the regression suite runs inside the CI container.
        self.assertIn("python3 test/test_merge_queue.py", step["run"])
        self.assertIn("nestor-ci:local", step["run"])

    def test_governance_relative_links_resolve(self) -> None:
        for document in GOVERNANCE_DOCS:
            for target in re.findall(r"\[[^]]+\]\(([^)]+)\)", document.read_text()):
                if target.startswith(("#", "http://", "https://", "mailto:")):
                    continue
                path = (document.parent / target.split("#", 1)[0]).resolve()
                with self.subTest(document=document.name, target=target):
                    self.assertTrue(path.exists(), f"broken relative link: {target}")


if __name__ == "__main__":
    unittest.main(verbosity=2)

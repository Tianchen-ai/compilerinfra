"""Read-only v0.5 parity and readiness inspection views."""

from __future__ import annotations

from collections import defaultdict
import json
from pathlib import Path
from typing import Any

from compilerinfra.performance import accuracy
from compilerinfra.tree import provenance, relative, tree


def _observed_hits_by_exit(observations) -> dict[str, set[str]]:
    hits: dict[str, set[str]] = defaultdict(set)
    for entry in (observations or {}).values():
        report = entry[0] if isinstance(entry, tuple) else entry
        ex = report.get("exit")
        if not ex:
            continue
        for ev in report.get("events", []):
            impl = ev.get("implementation")
            if impl:
                hits[ex].add(impl)
            if ev.get("action") == "candidate" and ev.get("details", {}).get("selected"):
                for bound in ev["details"].get("implementations", []):
                    hits[ex].add(bound)
                if ev["details"].get("model"):
                    hits[ex].add(ev["details"]["model"])
    return hits


def build_parity_report(scope, compiler: str = "base", exit_filter: str | None = None) -> dict[str, Any]:
    key = scope.key(compiler)
    desc = scope.values[key].description
    components = desc.get("components", {})
    units = desc.get("units", {})
    exits = desc.get("exits", {})
    selected_map = desc.get("selected", {})
    observed = _observed_hits_by_exit(scope.observations)

    target_exits = [exit_filter] if exit_filter else list(exits.keys())
    for ex in target_exits:
        if ex not in exits:
            raise ValueError(f"unknown exit {ex!r}")

    # 1. Explicit node capabilities -> per-exit parity (implemented / unsupported / todo).
    capabilities = {
        cid: meta for cid, meta in components.items() if meta.get("kind") == "capability"
    }
    by_exit_explicit: dict[str, list[dict[str, Any]]] = {}
    summary_counts: dict[str, dict[str, int]] = {}
    for ex in target_exits:
        rows = []
        counts = {"implemented": 0, "unsupported": 0, "todo": 0, "selected": 0, "observed_in_session": 0}
        for item in exits[ex].get("parity", []):
            status = item["status"]
            counts[status] = counts.get(status, 0) + 1
            if item.get("selected"):
                counts["selected"] += 1
            obs_hit = any(b in observed.get(ex, set()) for b in item.get("bindings", []))
            if obs_hit:
                counts["observed_in_session"] += 1
            rows.append({**item, "observed_in_session": obs_hit})
        by_exit_explicit[ex] = rows
        summary_counts[ex] = counts

    # 2. Sibling-exit structural & implementation alignment grouped by shared attachment node.
    attachment_groups: dict[str, dict[str, Any]] = {}
    by_parent: dict[str, list[str]] = defaultdict(list)
    for ex_name, ex_meta in exits.items():
        if ex_name in target_exits or exit_filter is None:
            parent_node = ex_meta["at"].split("@")[0]
            by_parent[parent_node].append(ex_name)

    families = {cid: m for cid, m in components.items() if m.get("kind") == "family"}
    for parent_node, group_exits in sorted(by_parent.items()):
        exit_summaries = {}
        for ex in group_exits:
            ex_unit_prefix = f"exits/{ex}"
            impls = {
                cid: m
                for cid, m in components.items()
                if m.get("kind") == "implementation"
                and units.get(m["unit"], {}).get("owner") == ex_unit_prefix
            }
            local_ests = [
                cid
                for cid, m in components.items()
                if m.get("kind") == "local_estimator"
                and units.get(m["unit"], {}).get("owner") == ex_unit_prefix
            ]
            active_set = set(selected_map.get(ex, []))
            reachable_families = [
                fid
                for fid, fm in families.items()
                if fm.get("at") in exits[ex].get("path", [])
            ]
            exit_summaries[ex] = {
                "valid": exits[ex].get("valid", False),
                "path": exits[ex].get("path", []),
                "families_registered": reachable_families,
                "families_selected": [f for f in reachable_families if f in active_set],
                "implementations_registered": len(impls),
                "implementation_operations": sorted({m["operation"] for m in impls.values()}),
                "typed_contracts": sum(1 for m in impls.values() if m.get("typed_contract")),
                "local_estimators": sorted(local_ests),
                "observed_hits_in_session": sorted(observed.get(ex, set())),
            }
        attachment_groups[parent_node] = {
            "exits": group_exits,
            "alignment": exit_summaries,
        }

    return {
        "mode": "read_only",
        "declared_capabilities": capabilities,
        "parity_by_exit": by_exit_explicit,
        "summary_by_exit": summary_counts,
        "sibling_groups": attachment_groups,
    }


def build_readiness_report(
    scope,
    compiler: str = "base",
    exit_filter: str | None = None,
    records: str | None = None,
) -> dict[str, Any]:
    key = scope.key(compiler)
    cand = scope.values[key]
    desc = cand.description
    components = desc.get("components", {})
    units = desc.get("units", {})
    exits = desc.get("exits", {})
    calibrations = desc.get("calibrations", {})

    target_exits = [exit_filter] if exit_filter else list(exits.keys())
    for ex in target_exits:
        if ex not in exits:
            raise ValueError(f"unknown exit {ex!r}")

    # Optional holdout accuracy records (v3 format), partitioned per exit.
    raw_records = None
    global_accuracy = None
    per_exit_accuracy: dict[str, Any] = {}
    if records:
        raw_records = json.loads(Path(records).read_text(encoding="utf-8"))
        global_accuracy = accuracy(raw_records)
        comps = raw_records.get("compilations", [])
        rows = raw_records.get("rows", [])
        batches_by_exit: dict[str, set[str]] = defaultdict(set)
        for c in comps:
            ex_name = c.get("prediction", {}).get("exit")
            if ex_name:
                batches_by_exit[ex_name].add(c["batch"])
        for ex in target_exits:
            bset = batches_by_exit.get(ex, set())
            if bset:
                sub_doc = {
                    "version": raw_records.get("version"),
                    "compilations": [c for c in comps if c["batch"] in bset],
                    "rows": [r for r in rows if r["batch"] in bset],
                }
                per_exit_accuracy[ex] = accuracy(sub_doc)

    # Adoption, workspace & one-step revert provenance.
    installed_fp = desc.get("fingerprint")
    workspace_fp = None
    workspace_matches = None
    if scope.profile == "development" and (scope.workspace / "product.json").is_file():
        try:
            workspace_fp = provenance(tree(scope.workspace))
            workspace_matches = workspace_fp == installed_fp
        except Exception as exc:
            workspace_fp = {"error": str(exc)}
            workspace_matches = False

    revert_info: dict[str, Any] = {"available": False, "adopted_fingerprint": None, "matches_installed": None}
    if scope.library and desc.get("library"):
        try:
            rel = relative(desc["library"])
            prefix = scope.library.parents[len(rel.parts) - 1]
            pointer = prefix / ".compilerinfra" / (rel.name + ".revert") / "previous"
            if pointer.is_symlink():
                saved = pointer.resolve(strict=True)
                rev_doc = json.loads((saved / "reverse.json").read_text(encoding="utf-8"))
                adopted_fp = rev_doc.get("adopted")
                revert_info = {
                    "available": True,
                    "adopted_fingerprint": adopted_fp,
                    "matches_installed": adopted_fp == installed_fp,
                }
        except Exception as exc:
            revert_info = {"available": False, "error": str(exc)}

    exit_reports: dict[str, Any] = {}
    for ex in target_exits:
        ex_meta = exits[ex]
        ex_owner = f"exits/{ex}"
        pr = ex_meta.get("print_rules", {"declarative": 0, "local": 0, "custom_count": 0, "custom": []})

        impls = {
            cid: m
            for cid, m in components.items()
            if m.get("kind") == "implementation"
            and units.get(m["unit"], {}).get("owner") == ex_owner
        }
        typed_impls = [cid for cid, m in impls.items() if m.get("typed_contract")]
        legacy_impls = [cid for cid, m in impls.items() if not m.get("typed_contract")]

        parity_list = ex_meta.get("parity", [])
        todo_caps = [p["capability"] for p in parity_list if p.get("status") == "todo"]
        unselected_caps = [
            p["capability"]
            for p in parity_list
            if p.get("status") == "implemented" and not p.get("selected")
        ]

        # Calibration provenance summary
        cal_profile = ex_meta.get("calibration_profile") or ""
        exit_cals = calibrations.get(ex, {})
        cal_doc = exit_cals.get(cal_profile) if cal_profile else None
        cal_sources = []
        if cal_doc and isinstance(cal_doc.get("facts"), dict):
            seen_src = set()
            for fval in cal_doc["facts"].values():
                src = fval.get("source") or {}
                sig = (src.get("device"), src.get("isa"), src.get("toolchain"), src.get("conditions"))
                if sig not in seen_src:
                    seen_src.add(sig)
                    cal_sources.append(
                        {
                            "device": src.get("device"),
                            "isa": src.get("isa"),
                            "toolchain": src.get("toolchain"),
                            "conditions": src.get("conditions"),
                        }
                    )

        acc_entry = per_exit_accuracy.get(ex)
        holdout_summary = (
            {
                "status": "measured",
                "holdout": acc_entry["holdout"],
                "calibration": acc_entry["calibration"],
                "excluded": acc_entry["excluded"],
            }
            if acc_entry is not None
            else {"status": "unmeasured", "holdout": None, "excluded": []}
        )

        missing = []
        if not ex_meta.get("valid", False):
            missing.append(f"exit_invalid: {ex_meta.get('error', 'unknown')}")
        if not cal_profile:
            missing.append("no_active_calibration_profile")
        if holdout_summary["status"] == "unmeasured":
            missing.append("holdout_accuracy_unmeasured")
        elif not (holdout_summary["holdout"] or {}).get("batches"):
            missing.append("no_holdout_split_batches")
        if pr.get("custom_count", 0) > 0:
            missing.append(f"custom_print_rules_present:{pr['custom_count']}")
        if legacy_impls:
            missing.append(f"legacy_string_contracts_present:{len(legacy_impls)}")
        if todo_caps:
            missing.append(f"parity_todo:{len(todo_caps)}")
        if workspace_matches is False:
            missing.append("workspace_fingerprint_diverged")

        exit_reports[ex] = {
            "valid": ex_meta.get("valid", False),
            "reuse": ex_meta.get("reuse", "vendor"),
            "numerical": ex_meta.get("numerical", {}),
            "v05_coverage": {
                "declarative_print_rules": pr.get("declarative", 0),
                "local_print_rules": pr.get("local", 0),
                "custom_print_rules_count": pr.get("custom_count", 0),
                "custom_print_rules": [c["operation"] for c in pr.get("custom", [])],
                "typed_contracts_count": len(typed_impls),
                "legacy_string_contracts_count": len(legacy_impls),
                "legacy_string_contracts": sorted(legacy_impls),
                "parity_todo": todo_caps,
                "parity_unselected": unselected_caps,
            },
            "calibration": {
                "selected_profile": cal_profile or None,
                "available_profiles": sorted(exit_cals.keys()),
                "version": cal_doc.get("version") if cal_doc else None,
                "fact_count": len(cal_doc.get("facts", {})) if cal_doc else 0,
                "provenance": cal_sources,
            },
            "model_accuracy": holdout_summary,
            "missing": missing,
        }

    return {
        "mode": "read_only",
        "provenance": {
            "installed_fingerprint": installed_fp,
            "workspace_fingerprint": workspace_fp,
            "workspace_matches_installed": workspace_matches,
            "revert": revert_info,
        },
        "global_model_accuracy": global_accuracy,
        "exits": exit_reports,
    }

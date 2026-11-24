"""Read-only v0.5 parity and readiness inspection views."""

from __future__ import annotations

from collections import defaultdict
import json
from pathlib import Path
from typing import Any

from compilerinfra.performance import accuracy
from compilerinfra.tree import relative


def _extract_arch(report: dict[str, Any], obs_desc: dict[str, Any] | None, fallback_desc: dict[str, Any]) -> str:
    ex = report.get("exit", "")
    cp = report.get("calibration_profile")
    if isinstance(cp, str) and cp:
        return cp
    if isinstance(obs_desc, dict):
        cp2 = obs_desc.get("exits", {}).get(ex, {}).get("calibration_profile")
        if isinstance(cp2, str) and cp2:
            return cp2
    ex_opts = (
        report.get("configuration", {})
        .get("exits", {})
        .get(ex, {})
        .get("options", {})
    )
    if isinstance(ex_opts, dict):
        for k in ("calibration", "architecture", "arch", "profile"):
            val = ex_opts.get(k)
            if isinstance(val, str) and val:
                return val
    cp3 = fallback_desc.get("exits", {}).get(ex, {}).get("calibration_profile")
    if isinstance(cp3, str) and cp3:
        return cp3
    return "*"


def _collect_winning_receipt_hits(
    report: dict[str, Any],
    cand_key: str,
    arch: str,
    hits: dict[tuple[str, str, str], set[str]],
) -> None:
    if not isinstance(report, dict) or report.get("error"):
        return
    ex = report.get("exit")
    if not ex:
        return
    bucket = hits[(ex, cand_key, arch)]
    for dec in report.get("identity", {}).get("decisions", []):
        if dec.get("family"):
            bucket.add(dec["family"])
    for ev in report.get("events", []):
        act = ev.get("action")
        if act == "candidate":
            details = ev.get("details") or {}
            if details.get("selected") is True and not details.get("eliminated"):
                if ev.get("family"):
                    bucket.add(ev["family"])
                if ev.get("implementation"):
                    bucket.add(ev["implementation"])
                for bound in details.get("implementations", []):
                    bucket.add(bound)
                if details.get("model"):
                    bucket.add(details["model"])
        elif act == "materialize":
            if ev.get("implementation"):
                bucket.add(ev["implementation"])
        elif act == "contribution" and ev.get("executed") is True:
            if ev.get("implementation"):
                bucket.add(ev["implementation"])


def _observed_hits_by_exit_candidate_arch(
    observations,
    target_candidate_key: str,
    target_desc: dict[str, Any],
    records_doc: dict[str, Any] | None = None,
) -> dict[tuple[str, str, str], set[str]]:
    """Partition winning selection receipts by (exit, candidate, architecture)."""
    hits: dict[tuple[str, str, str], set[str]] = defaultdict(set)
    target_fp = target_desc.get("fingerprint")
    for entry in (observations or {}).values():
        if isinstance(entry, tuple):
            report = entry[0]
            obs_desc = entry[1] if len(entry) > 1 and isinstance(entry[1], dict) else None
        else:
            report = entry
            obs_desc = None
        if not isinstance(report, dict):
            continue
        obs_cand = report.get("scope_candidate")
        if obs_cand is not None:
            if obs_cand != target_candidate_key:
                continue
        elif obs_desc is not None and obs_desc.get("fingerprint") != target_fp:
            continue
        elif "fingerprint" in report and target_fp is not None and report["fingerprint"] != target_fp:
            continue
        arch = _extract_arch(report, obs_desc, target_desc)
        _collect_winning_receipt_hits(report, target_candidate_key, arch, hits)

    if isinstance(records_doc, dict):
        for comp in records_doc.get("compilations", []):
            if not isinstance(comp, dict):
                continue
            for field in ("default_prediction", "prediction"):
                rep = comp.get(field)
                if not isinstance(rep, dict):
                    continue
                if target_fp is not None and rep.get("fingerprint") != target_fp:
                    continue
                if target_candidate_key == "base" and rep.get("product_default") is not True:
                    continue
                if target_candidate_key != "base" and rep.get("candidate") not in (target_candidate_key, None):
                    continue
                arch = _extract_arch(rep, None, target_desc)
                _collect_winning_receipt_hits(rep, target_candidate_key, arch, hits)
    return hits


def _hits_for_parity_row(
    hits: dict[tuple[str, str, str], set[str]],
    ex: str,
    cand_key: str,
    row_arch: str,
) -> set[str]:
    if not row_arch or row_arch == "*":
        merged: set[str] = set()
        for (e, c, _a), vals in hits.items():
            if e == ex and c == cand_key:
                merged |= vals
        return merged
    return set(hits.get((ex, cand_key, row_arch), set()))


def _hits_by_arch_for_exit(
    hits: dict[tuple[str, str, str], set[str]],
    ex: str,
    cand_key: str,
) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for (e, c, a), vals in sorted(hits.items()):
        if e == ex and c == cand_key and vals:
            out[a] = sorted(vals)
    return out


def build_parity_report(
    scope,
    compiler: str = "base",
    exit_filter: str | None = None,
    records: str | None = None,
) -> dict[str, Any]:
    key = scope.key(compiler)
    desc = scope.values[key].description
    components = desc.get("components", {})
    units = desc.get("units", {})
    exits = desc.get("exits", {})
    configured_map = desc.get("selected", {})
    records_doc = json.loads(Path(records).read_text(encoding="utf-8")) if records else None
    observed_winners = _observed_hits_by_exit_candidate_arch(
        scope.observations, key, desc, records_doc
    )

    target_exits = [exit_filter] if exit_filter else list(exits.keys())
    for ex in target_exits:
        if ex not in exits:
            raise ValueError(f"unknown exit {ex!r}")

    capabilities = {
        cid: meta for cid, meta in components.items() if meta.get("kind") == "capability"
    }
    by_exit_explicit: dict[str, list[dict[str, Any]]] = {}
    summary_counts: dict[str, dict[str, int]] = {}
    for ex in target_exits:
        rows = []
        counts = {
            "implemented": 0,
            "unsupported": 0,
            "todo": 0,
            "configured": 0,
            "selected": 0,
        }
        for item in exits[ex].get("parity", []):
            status = item["status"]
            counts[status] = counts.get(status, 0) + 1
            if item.get("configured"):
                counts["configured"] += 1
            bindings = item.get("bindings", [])
            row_arch = item.get("architecture") or "*"
            arch_hits = _hits_for_parity_row(observed_winners, ex, key, row_arch)
            real_selected = (
                status == "implemented"
                and bool(bindings)
                and any(b in arch_hits for b in bindings)
            )
            if real_selected:
                counts["selected"] += 1
            rows.append({**item, "candidate": key, "selected": real_selected})
        by_exit_explicit[ex] = rows
        summary_counts[ex] = counts

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
            configured_set = set(configured_map.get(ex, []))
            reachable_families = [
                fid
                for fid, fm in families.items()
                if fm.get("at") in exits[ex].get("path", [])
            ]
            any_arch_hits = _hits_for_parity_row(observed_winners, ex, key, "*")
            by_arch_hits = _hits_by_arch_for_exit(observed_winners, ex, key)
            exit_summaries[ex] = {
                "candidate": key,
                "valid": exits[ex].get("valid", False),
                "path": exits[ex].get("path", []),
                "families_registered": reachable_families,
                "families_configured": [f for f in reachable_families if f in configured_set],
                "families_selected_in_receipts": [
                    f for f in reachable_families if f in any_arch_hits
                ],
                "implementations_registered": len(impls),
                "implementation_operations": sorted({m["operation"] for m in impls.values()}),
                "typed_contracts": sum(1 for m in impls.values() if m.get("typed_contract")),
                "local_estimators": sorted(local_ests),
                "winning_receipt_hits_by_arch": by_arch_hits,
            }
        attachment_groups[parent_node] = {
            "exits": group_exits,
            "alignment": exit_summaries,
        }

    return {
        "mode": "read_only",
        "candidate": key,
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

    observed_winners = _observed_hits_by_exit_candidate_arch(
        scope.observations, key, desc, raw_records
    )

    # Revert evidence must match the CURRENT installed fingerprint; stale
    # receipts from an earlier installation do NOT count as verified.
    installed_fp = desc.get("fingerprint")
    revert_info: dict[str, Any] = {
        "verified": False,
        "stale": False,
        "matches_installed": False,
        "pending_adoption": False,
        "reverted_from": None,
        "restored_fingerprint": None,
    }
    if scope.library and desc.get("library"):
        try:
            rel = relative(desc["library"])
            prefix = scope.library.parents[len(rel.parts) - 1]
            rev_dir = prefix / ".compilerinfra" / (rel.name + ".revert")
            pointer = rev_dir / "previous"
            last_rev = rev_dir / "last_revert.json"
            revert_info["pending_adoption"] = pointer.is_symlink()
            if last_rev.is_file():
                rev_doc = json.loads(last_rev.read_text(encoding="utf-8"))
                reverted_from = rev_doc.get("reverted_from")
                restored_fp = rev_doc.get("restored_fingerprint")
                revert_info["reverted_from"] = reverted_from
                revert_info["restored_fingerprint"] = restored_fp
                matches = bool(installed_fp is not None and restored_fp == installed_fp)
                revert_info["matches_installed"] = matches
                if (
                    isinstance(reverted_from, dict)
                    and isinstance(restored_fp, dict)
                    and reverted_from != restored_fp
                ):
                    if matches and not revert_info["pending_adoption"]:
                        revert_info["verified"] = True
                    else:
                        revert_info["stale"] = not matches
        except Exception as exc:
            revert_info = {"verified": False, "error": str(exc)}

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
            if p.get("status") == "implemented"
            and not any(
                b in _hits_for_parity_row(observed_winners, ex, key, p.get("architecture") or "*")
                for b in p.get("bindings", [])
            )
        ]

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
        if not revert_info.get("verified"):
            missing.append("revert_unverified_or_stale" if revert_info.get("stale") else "revert_unverified")
        if pr.get("custom_count", 0) > 0:
            missing.append(f"custom_print_rules_present:{pr['custom_count']}")
        if legacy_impls:
            missing.append(f"legacy_string_contracts_present:{len(legacy_impls)}")
        if todo_caps:
            missing.append(f"parity_todo:{len(todo_caps)}")

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
        "candidate": key,
        "provenance": {
            "installed_fingerprint": installed_fp,
            "revert": revert_info,
        },
        "global_model_accuracy": global_accuracy,
        "exits": exit_reports,
    }

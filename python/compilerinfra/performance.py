"""Prediction/measurement interchange used by external Bench implementations."""

from collections import defaultdict
from itertools import combinations
from math import isfinite
from numbers import Real
from statistics import mean
import re


def _number(value, positive=False):
    return (
        isinstance(value, Real)
        and not isinstance(value, bool)
        and isfinite(value)
        and (value > 0 if positive else value >= 0)
    )


def _top_k(values, k):
    """Fractional membership for a tie crossing the kth position."""
    if not values:
        return {}
    cutoff = sorted(values.values())[min(k, len(values)) - 1]
    before = sum(value < cutoff for value in values.values())
    tied = sum(value == cutoff for value in values.values())
    share = min(1, (k - before) / tied)
    return {
        key: (1 if value < cutoff else share if value == cutoff else 0)
        for key, value in values.items()
    }


def _join_batch(batch, compilations, measured):
    """Use the selected proposal in each actual compilation as the prediction side."""
    default = compilations[0]["default_prediction"]
    family = compilations[0]["family"]
    def choices(report):
        return [e["details"] for e in report["events"] if e["action"] == "candidate"
                and e["family"] == family and e["details"]["selected"]]
    picked = choices(default)
    if default["product_default"] is not True or len(picked) != 1 or not default["package_sha256"]:
        raise ValueError("batch requires an observed default compilation for this decision")
    default_structure = picked[0]["candidate"]
    expected = {}
    for entry in compilations:
        if entry["family"] != family or entry["default_prediction"] != default:
            raise ValueError("inconsistent default observation or family")
        p = entry["prediction"]
        cid, compiler = p["compilation_id"], p["candidate"]
        if any(p[k] != default[k] for k in ("root_sha256","exit","fingerprint")):
            raise ValueError("different build, exit or input in default and candidate")
        def target_options(report):
            return report["configuration"].get("exits", {}).get(report["exit"], {}).get("options", {})
        if target_options(p) != target_options(default):
            raise ValueError("candidate target differs from default compilation")
        if not all(isinstance(v, str) and v for v in (cid, compiler, p["exit"])):
            raise ValueError("missing compiler or compilation identity")
        if not re.fullmatch("[0-9a-f]{64}", p["artifact_sha256"]):
            raise ValueError("invalid emitted artifact identity")
        choices = [event["details"] for event in p["events"]
                   if event["action"] == "candidate" and event["family"] == entry["family"] and event["details"]["selected"]]
        if len(choices) != 1 or not choices[0]["rows"]:
            raise ValueError("expected one realized candidate and its complete prediction row domain")
        choice = choices[0]
        for key, prediction in choice["rows"].items():
            ident = (choice["candidate"], str(key))
            if ident in expected:
                raise ValueError(f"duplicate prediction {ident}")
            expected[ident] = (p, prediction, choice["predicted_ns"])
    seen = set()
    joined = []
    for row in measured:
        ident = (row["candidate"], str(row["row_id"]))
        if ident in seen or ident not in expected:
            raise ValueError(f"unexpected or repeated measured row {ident}")
        seen.add(ident)
        p, pred, ns = expected[ident]
        receipt = row["runtime_identity"]
        if (receipt["compilation_id"], receipt["compiler_candidate"], receipt["package_sha256"]) != (
                p["compilation_id"], p["candidate"], p["package_sha256"]):
            raise ValueError(f"{ident}: measured package or native identity mismatch")
        if (receipt["structures"].get(family), str(receipt["row_id"])) != ident:
            raise ValueError(f"{ident}: measured structure or row differs")
        selected = ident[0] == default_structure
        if "selected" in row and row["selected"] is not selected:
            raise ValueError(f"{ident}: reported selection disagrees with default compilation")
        if (row["compilation_id"], row["compiler_candidate"]) != (p["compilation_id"], p["candidate"]):
            raise ValueError(f"{ident}: compilation or candidate identity mismatch")
        if row["artifact_sha256"] != p["artifact_sha256"]:
            raise ValueError(f"{ident}: measured artifact differs from emitted artifact")
        joined.append(dict(row, selected=selected, terms=pred["terms"], predicted_ns=pred["predicted_ns"], structure_ns=ns))
    if expected.keys() - seen:
        raise ValueError(f"missing measured rows {sorted(expected.keys()-seen)}")
    return joined


def accuracy(document, k=1):
    """Score only complete batches paired with independent compiler reports."""
    if document.get("version") != 3:
        raise ValueError("cost records require version 3 with measured package identities and default explain")
    if type(k) is not int or k <= 0:
        raise ValueError("k must be positive")
    grouped = defaultdict(list)
    predictions = defaultdict(list)
    ids = {}
    conflicts = set()
    for e in document["compilations"]:
        predictions[e["batch"]].append(e)
        p = e["prediction"]
        cid = p.get("compilation_id")
        if cid in ids and ids[cid] != p:
            conflicts.add(cid)
        ids[cid] = p
    for row in document["rows"]:
        grouped[row["batch"]].append(row)
    accepted, excluded = [], []
    for batch in sorted(predictions.keys() | grouped.keys()):
        try:
            if not predictions[batch] or not grouped[batch]:
                raise ValueError("batch lacks predictions or measurements")
            if any(e["prediction"].get("compilation_id") in conflicts for e in predictions[batch]):
                raise ValueError("conflicting records for compilation ID")
            inputs = {(e["prediction"]["root_sha256"], e["prediction"]["exit"]) for e in predictions[batch]}
            if len(inputs)!=1:
                raise ValueError("batch mixes inputs or exits")
            rows = _join_batch(batch, predictions[batch], grouped[batch])
            _score(rows, k)
            accepted.extend(rows)
        except (ValueError, TypeError, KeyError) as error:
            excluded.append({"batch":batch, "reason":str(error)})
    return dict(_score(accepted, k), excluded=excluded)


def _score(measurements, k):
    batches = defaultdict(lambda: defaultdict(dict))
    batch_splits = {}
    for row in measurements:
        batch, split, candidate, row_id = (
            row[n] for n in ("batch", "split", "candidate", "row_id")
        )
        if split not in ("calibration", "holdout") or any(
            not isinstance(v, str) or not v for v in (batch, candidate)
        ):
            raise ValueError("incomplete batch, candidate, or split")
        if batch_splits.setdefault(batch, split) != split:
            raise ValueError(
                f"{batch}: batch overlaps the calibration and holdout sets"
            )
        if type(row_id) not in (str, int) or not re.fullmatch(
            r"[0-9a-f]{64}", row["artifact_sha256"]
        ):
            raise ValueError(f"{batch}: invalid row ID or artifact SHA-256")
        if type(row["winner"]) is not bool or type(row["selected"]) is not bool:
            raise ValueError(f"{batch}: expected boolean winner and selected flags")
        if not _number(row["measured_ns"], True) or (
            row["predicted_ns"] is not None and not _number(row["predicted_ns"])
        ):
            raise ValueError(f"{batch}: invalid latency")
        terms = row["terms"]
        if not isinstance(terms, dict) or any(
            not item.get("unit") or not _number(item["value"])
            for item in terms.values()
        ):
            raise ValueError(f"{batch}: invalid unitful prediction components")
        entries = batches[batch][candidate]
        if row_id in entries:
            raise ValueError(f"{batch}/{candidate}: duplicate row {row_id}")
        entries[row_id] = row

    def summarize(split):
        correct = pairs = 0
        recalls, ratios = [], []
        unknown = 0
        details = []
        for batch, candidates in batches.items():
            if batch_splits[batch] != split:
                continue
            structures = []
            for name, rows in candidates.items():
                winners = [r for r in rows.values() if r["winner"]]
                if (
                    len(winners) != 1
                    or len({r["selected"] for r in rows.values()}) != 1
                ):
                    raise ValueError(
                        f"{batch}/{name}: inconsistent selection or runtime winner"
                    )
                # Structural prediction is the best predicted legal row, independent
                # of which row won tuning. Time the complete call with its actual winner.
                predictions = [r["predicted_ns"] for r in rows.values()]
                predicted = (
                    min(predictions)
                    if all(p is not None for p in predictions)
                    else None
                )
                totals = {r.get("structure_ns") for r in rows.values()}
                if len(totals) != 1:
                    raise ValueError(
                        f"{batch}/{name}: inconsistent structural estimate"
                    )
                total = totals.pop()
                if total is not None:
                    if not _number(total):
                        raise ValueError(f"{batch}/{name}: invalid structural estimate")
                    predicted = total
                structures.append(
                    (name, predicted, winners[0]["measured_ns"], winners[0]["selected"])
                )
            selected = [s for s in structures if s[3]]
            if len(selected) != 1:
                raise ValueError(f"{batch}: expected one compiler-selected structure")
            best = min(s[2] for s in structures)
            ratio = selected[0][2] / best
            ratios.append(ratio)
            known = sorted(
                (s for s in structures if s[1] is not None), key=lambda s: s[1]
            )
            unknown += len(structures) - len(known)
            for a, b in combinations(known, 2):
                if a[2] == b[2]:
                    continue
                pairs += 1
                correct += 0.5 if a[1] == b[1] else int((a[1] < b[1]) == (a[2] < b[2]))
            predicted_set = _top_k({s[0]: s[1] for s in known}, k)
            measured_set = _top_k({s[0]: s[2] for s in structures}, k)
            recall = sum(
                min(predicted_set.get(key, 0), membership)
                for key, membership in measured_set.items()
            ) / min(k, len(structures))
            recalls.append(recall)
            details.append(
                {
                    "batch": batch,
                    "selected": selected[0][0],
                    "selected_over_best": ratio,
                    "top_k_recall": recall,
                }
            )
        return {
            "pairs": pairs,
            "pairwise_accuracy": correct / pairs if pairs else None,
            "top_k_recall": mean(recalls) if recalls else None,
            "unknown_predictions": unknown,
            "selected_over_best": mean(ratios) if ratios else None,
            "batches": details,
        }

    return {split: summarize(split) for split in ("calibration", "holdout")}

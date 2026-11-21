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


def accuracy(document, k=1):
    """Evaluate per-structure batch latency. Bench supplies every row and its winner."""
    if document.get("version") != 1 or not isinstance(document.get("rows"), list):
        raise ValueError("expected version 1 of the cost record with a rows array")
    if type(k) is not int or k <= 0:
        raise ValueError("k must be a positive integer")
    batches = defaultdict(lambda: defaultdict(dict))
    batch_splits = {}
    for row in document["rows"]:
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

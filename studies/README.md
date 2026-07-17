# Studies

Measurement studies run with this fork of qperf. Each is self-contained: the report,
the raw data it was derived from, the scripts that produced and analysed it, and a
runbook to recreate it.

The rule that makes these worth keeping: **a study's conclusions must be
re-derivable from its own directory.** If the numbers in a report cannot be
regenerated from the committed data by running the committed scripts, the study is
incomplete — a report alone is a claim, not a result.

## Index

| study | question | finding |
|---|---|---|
| [parallel-streams-vs-connections](parallel-streams-vs-connections/) | Do N parallel QUIC streams over one connection increase throughput? | **No** — 0.93x from N=1 to N=8, while N parallel TCP connections give 4.92x on the same path. Streams share one congestion window. (2026-07-16) |

## Layout of a study

```
studies/<topic-name>/
  README.md          the report: question, method, results, interpretation, limits
  REPRODUCING.md     runbook to recreate it from scratch
  scripts/           sweeps + an analyzer that regenerates the report's tables
  data/<date>-<host>/  raw output of one measurement campaign, with a provenance README
```

Named by **topic**, not date — the same question can be re-measured on a different
link or a later day, and that is a new dated directory under `data/`, not a new study.
The analyzer takes a data directory as its argument for exactly this reason:

```
studies/parallel-streams-vs-connections/scripts/analyze.py \
  studies/parallel-streams-vs-connections/data/2026-07-16-weshootfilm
```

## Adding a study

1. `studies/<topic-name>/`, with the layout above.
2. Write the report so its **limitations** are as easy to find as its headline. The
   existing study is a good template: it records an open confound, a metric bug found
   during analysis, and a hypothesis it refuted along the way.
3. Commit the raw data with the report. It is small, and without it the report cannot
   be checked, re-analysed with a different metric, or corrected.
4. Add a row to the index above.

<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/policy-engine.md
Purpose : Retention rule grammar, matching semantics, precedence algorithm and expiry computation
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Retention policy engine

Sources: `src/policy/policy_engine.{hpp,cpp}`, `src/policy/glob.{hpp,cpp}`,
`src/config/config_loader.cpp` (`parse_retention`, `parse_rule`). Tests:
`tests/policy/test_policy.cpp`, `tests/config/test_config.cpp`. User-level
reference: [configuration](../user/configuration.md). Related:
[garbage collector](garbage-collector.md).

## 1. Rule grammar

```yaml
retention:
  default: "never" | <duration>          # files matched by no rule (default: "never")
  timestamp: modification | birth        # alias key: evaluation_timestamp (not both)
  rules:
    - id: <[A-Za-z0-9._-]{1,64}, unique>  # required
      enabled: true | false               # default true
      action: delete | exclude            # default delete ("expire" internally)
      priority: <integer -1000..1000>     # default 0
      # scope (at most one of file / path / path_glob; see constraints)
      file: "/exact/managed/path"
      path: "/directory"                  # direct children only...
      recursive: true | false             # ...or the whole subtree (default false)
      path_glob: "/dir/*/sub/**/*.bin"
      # pattern on the basename
      glob: "*.mp4"
      extension: "mp4" | ["csv", "tar.gz"]
      # lifetime (delete rules only)
      retention: "never" | <duration>     # required for delete rules
      timestamp: modification | birth     # overrides retention.timestamp
```

* Durations: `<digits><unit>` groups with units `w d h m s ms`, each unit at
  most once, from the largest to the smallest (`"1d12h"`), no sign, no
  fraction, overflow rejected (`parse_duration`, `src/common/units.cpp`).
  `retention` must be `"never"` or at least `1s`.
* `timestamp` accepts `modification` (alias `last_modified`) and `birth`
  (alias `creation`).
* Paths (`file`, `path`) are managed paths: they must start with `/`; repeated
  slashes are collapsed and a trailing slash removed; `.` and `..` components
  are rejected (`managed_path::normalize`).
* `extension`: a leading dot is removed; empty values, values containing
  `/ * ? [ ] \`, or starting/ending with a dot are rejected; stored lower case.
* `glob` and `path_glob` are compiled at load time; an invalid pattern is a
  configuration error with its line and column.

Constraints enforced by `parse_rule`:

| Constraint | Error |
|---|---|
| At least one of `file`, `path`, `glob`, `path_glob`, `extension` | "a rule needs at least one of ..." |
| `file` excludes `path`, `glob`, `path_glob`, `extension`; `file` cannot be `/` | |
| `path_glob` excludes `path` | "put the directory in the pattern" |
| `recursive` only with `path` | |
| `exclude` rules take neither `retention` nor `timestamp` | |
| `delete` rules require `retention` | |
| Unknown or duplicate keys, duplicate `id` | rejected |

## 2. Rule types and matching

A rule matches a file when **every** criterion it specifies matches
(`CompiledRule::matches`). Only regular files are ever evaluated: the GC calls
the engine for `S_ISREG` entries only, so directories, symlinks and special
files never match anything.

| Criterion | Matches when | Example |
|---|---|---|
| `file: F` | the managed path equals `F` exactly | `file: /tmp/one.bin` matches `/tmp/one.bin`, not `/tmp/sub/one.bin` |
| `path: D` (non-recursive) | the file's parent directory equals `D` | `path: /tmp/record` matches `/tmp/record/a`, not `/tmp/record/sub/a` |
| `path: D, recursive: true` | the parent is `D` or below it, component-wise | `/cache` matches `/cache/a` and `/cache/x/y/z`; `/tmp/record` does not match `/tmp/recordings/a` |
| `glob: G` | the basename matches `G` (basename mode) | `*.core` |
| `path_glob: P` | the whole managed path matches `P` component by component | `/sessions/session-*/**` |
| `extension: [E...]` | the basename ends with `.E` for one `E`, ASCII case-insensitive, **with at least one byte before the dot** | `mp4` matches `a.mp4`, `VIDEO.MP4`; not `.mp4`, not `a.mp4.part` |

A directory is never "a file of its own scope": `path: /cache, recursive: true`
does not match a file named `/cache` (`DirectoryNonRecursiveVersusRecursive`).

Criteria combine with AND: `path: /tmp/record, recursive: true, glob: "*.mp4"`
matches `/tmp/record/x/a.mp4` but not `/tmp/record/a.mkv` nor `/other/a.mp4`.

For `path_glob`, the engine first checks that the path is within the pattern's
longest wildcard-free directory prefix (`Glob::literal_prefix`, e.g. `/var/log`
for `/var/log/*/x.log`) before running the component matcher.

## 3. Glob semantics (`src/policy/glob.cpp`)

Globs are never regular expressions. Matching is case-sensitive and allocation
free; patterns are compiled once per configuration generation.

| Token | Meaning |
|---|---|
| `*` | any sequence of bytes inside one component, including the empty sequence |
| `?` | exactly one character: one valid UTF-8 sequence, or one byte if the input is not valid UTF-8 there |
| `[abc]`, `[a-z]`, `[!a-z]`, `[^a-z]` | one byte in / not in the set (byte oriented: use for ASCII). `]` first in the set is literal; `-` last is literal; a reversed range is an error |
| `\x` | the character `x` literally |
| `**` | path globs only, and only as a whole component: zero or more components |

* **Hidden files are not special.** `*` matches a leading dot: `*` matches
  `.hidden`, and `*.mp4` matches `.mp4` (the star matches the empty string)
  (`Glob.BasenameWildcards`). This differs from shell globbing.
* `*` never crosses `/` in a path glob: `/rec/*.mp4` matches `/rec/a.mp4` but
  not `/rec/sub/a.mp4`.
* `**` may match zero components: `/rec/**/a.mp4` matches `/rec/a.mp4` and
  `/rec/x/y/z/a.mp4`; `/**` matches everything. A trailing `**` therefore also
  matches a file whose path is the parent itself: `/sessions/session-*/**`
  matches a regular file named `/sessions/session-7`.
* Consecutive `*` inside a component collapse to one. Matching uses a linear
  algorithm with a single backtracking point, both inside a component (`*`)
  and across components (`**`), so there is no exponential backtracking.
* Rejected at compile time: empty pattern, NUL byte, unterminated class,
  trailing backslash, reversed range, `/` or `**` in a basename glob, a path
  glob not starting with `/`, ending with `/`, with an empty, `.` or `..`
  component, or with `**` not forming a whole component.

## 4. Precedence algorithm

`PolicyEngine::compile` (`src/policy/policy_engine.cpp`) computes, for each
**enabled** rule, a specificity key compared lexicographically (larger wins):

```
key = (priority, scope_rank, depth, non_recursive, pattern_rank, literals)
```

| Field | Value |
|---|---|
| `priority` | `rule.priority` (-1000..1000) |
| `scope_rank` | `file` = 3, `path_glob` = 2, `path` = 1, none = 0 |
| `depth` | components of `file`; of `path`; of the path glob's literal prefix; 0 otherwise |
| `non_recursive` | 1 for `file` and for non-recursive `path`; 0 otherwise |
| `pattern_rank` | 1 when the rule has a `glob` or an `extension`; 0 otherwise |
| `literals` | max of: the glob's literal character count; (shortest extension length + 1); the path glob's literal character count |

Rules are sorted by key, descending; **ties keep declaration order**. Exclude
rules and delete rules are stored in two separate lists, each in that order.
Disabled rules are dropped (but their ids remain addressable by index).

`PolicyEngine::evaluate(path, times)`:

1. Scan the **exclude** list; the first match returns `Decision::excluded`.
   An enabled exclusion that matches always wins, whatever the priorities
   (`ExclusionAlwaysWinsEvenAgainstHigherPriority`: a priority-1000 delete
   rule loses against a priority -1000 exclusion).
2. Scan the **delete** list; the first match decides: `keep_forever` if its
   retention is `never`, otherwise `expires`.
3. No match: `retention.default` decides with `retention.timestamp`
   (`rule_index = -1`, reported as rule `"default"`).

Evaluation is a linear scan stopping at the first match, over rules already in
precedence order; nothing is compiled at evaluation time and matching does not
allocate (except for path globs deeper than 64 components). The engine is
immutable and shared read-only (`std::shared_ptr<const PolicyEngine>`).

Consequences of the key order, each covered by a test:

* Priority beats specificity: a priority-1 `extension: mp4` rule beats a
  priority-0 `file` rule (`PriorityBeatsSpecificity`).
* At equal priority: `file` > `path_glob` > `path` > pattern-only; deeper
  directory > shallower; non-recursive > recursive at the same depth
  (`MoreSpecificScopeWins`, `NonRecursiveBeatsRecursiveAtSameDepth`).
* At equal scope: a rule with a glob/extension beats one without; more literal
  characters win (`rec-*.mp4` beats `*.mp4`); identical keys fall back to
  declaration order (`MorePatternLiteralsWinThenDeclarationOrder`).

## 5. Timestamp sources

`FileTimes` (`src/policy/policy_engine.hpp`) carries `modification` (statx
`stx_mtime`) and an optional `birth` (statx `stx_btime`), both truncated to
milliseconds (`src/fsutil/entry_stat.cpp`).

* `modification` (default): `st_mtime`, the last content modification. Writes,
  truncation, `fallocate` and `utimens`/`touch` move it, and through the mount
  they also notify the GC.
* `birth`: statx birth time when the backing filesystem reports it
  (`STATX_BTIME` in `stx_mask`). `STATX_BTIME` is requested only when at least
  one enabled delete rule with a retention, or the default, uses `birth`
  (`PolicyEngine::needs_birth_time`).
* **Fallback**: when `birth` is requested but unavailable, `st_mtime` is used
  and `Decision::birth_fallback` is set (`BirthTimestampAndFallback`).
* `ctime` (inode change time) is never used as a creation time.

## 6. Expiration semantics

* `expiry = base + retention`, computed with `saturating_add`, which uses C++26
  `std::add_sat` on the millisecond counts: an enormous retention clamps to
  `TimePoint::max()` instead of overflowing into the past
  (`ExpiryComputationSaturates`).
* A file is expired when `expiry <= now` (`Decision::expired_at`): exactly at
  the expiry instant counts as expired; one millisecond before does not
  (`NotExpiredExactlyAtAndAfterExpiry`, `GcTest.ExactlyAtExpiryIsDeleted`,
  `GcTest.OneMillisecondBeforeExpiryIsKept`).
* `now` is the wall clock (`Clock::now`, `src/common/clock.hpp`). A timestamp in
  the future simply expires later (`FutureModificationTimeExpiresLater`); a
  wall-clock step backwards delays deletions and never advances them
  (`GcTest.ClockStepBackwardsDoesNotDeleteEarly`).
* `never` (rule or default) yields `keep_forever`; with no rule and the default
  `never`, `PolicyEngine::never_expires()` is true.
* The decision is recomputed from fresh `statx` data immediately before every
  deletion (see [garbage-collector.md](garbage-collector.md#6-validation-before-unlink)).

## 7. Worked examples

Rules of `config/config.example.yml` (the disabled `archived-exports` rule is
ignored), with their keys and resulting order:

| Index | id | Criteria | Key | Order |
|---|---|---|---|---|
| 0 | temporary-recordings | `path: /tmp/record`, recursive, 1d | (0,1,2,0,0,0) | 4th |
| 1 | video-files | `extension: mp4`, 1d | (0,0,0,0,1,4) | 6th |
| 2 | temporary-cache | `path: /var/cache/tsfs`, recursive, 30m | (0,1,3,0,0,0) | 2nd |
| 3 | application-logs | `path: /var/log/application`, recursive, 7d | (0,1,3,0,0,0) | 3rd (tie with 2, declared later) |
| 4 | core-dumps | `glob: "*.core"`, 2d | (0,0,0,0,1,5) | 5th |
| 5 | sessions | `path_glob: /sessions/session-*/**`, 12h, birth | (0,2,1,0,0,16) | 1st |
| 6 | keep-important-recordings | exclude, `path: /tmp/record/keep`, recursive | exclusion list | |

Default: `never`, timestamp `modification`. Assume a file whose `st_mtime` is
2026-10-13T10:00:00Z.

| Managed path | Deciding rule | Decision |
|---|---|---|
| `/tmp/record/cam1/video.mp4` | temporary-recordings (path beats the extension rule) | expires 2026-10-14T10:00:00Z |
| `/tmp/record/keep/a.mp4` | keep-important-recordings | excluded (never deleted) |
| `/home/u/MOVIE.MP4` | video-files (case-insensitive extension) | expires 2026-10-14T10:00:00Z |
| `/home/u/.mp4` | none (an extension needs a stem) | default: keep forever |
| `/var/cache/tsfs/x.core` | temporary-cache (scope beats a bare glob) | expires 2026-10-13T10:30:00Z |
| `/data/app.core` | core-dumps | expires 2026-10-15T10:00:00Z |
| `/data/.core` | core-dumps (`*` matches the empty string, hidden names are not special) | expires 2026-10-15T10:00:00Z |
| `/sessions/session-42/tmp/a.mp4` | sessions (highest scope rank) | 12h after birth time; after `st_mtime` if the filesystem has no birth time |
| `/other/readme.txt` | none | default: keep forever |

The precedence principles used here are those exercised by
`tests/policy/test_policy.cpp`; the effective decision for a real tree can be
listed without deleting anything with `tsfs --dry-run`.

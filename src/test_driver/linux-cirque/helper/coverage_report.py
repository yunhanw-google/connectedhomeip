"""
Copyright (c) 2026 Project CHIP Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import argparse
import json
import logging
import mmap
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

logger = logging.getLogger(__name__)

SUBSYSTEMS = [
    ("src/app", "src/app/"),
    ("src/transport", "src/transport/"),
    ("src/protocols", "src/protocols/"),
    ("src/ble", "src/ble/"),
    ("src/inet", "src/inet/"),
    ("src/platform/Linux", "src/platform/Linux/"),
    ("src/controller", "src/controller/"),
    ("examples", "examples/"),
]

EXCLUDE_PATTERNS = [
    "*/zzz_generated/*",
    "*/connectedhomeip/third_party/*",
    "/usr/*",
    "*/src/app/util/mock/*",
    "*/tests/*",
    "*/testing/*",
]



def parse_lcov_info(info_content: str) -> Dict[str, Dict[str, int]]:
    """
    Parses lcov/info tracefile content into file-level line and function coverage statistics.
    Returns:
        Dict[source_path, {"lines_found": int, "lines_hit": int, "functions_found": int, "functions_hit": int}]
    """
    files_stats = {}
    current_file = None
    lines_found = 0
    lines_hit = 0
    functions_found = 0
    functions_hit = 0

    for line in info_content.splitlines():
        line = line.strip()
        if line.startswith("SF:"):
            current_file = line[3:].strip()
            lines_found = 0
            lines_hit = 0
            functions_found = 0
            functions_hit = 0
        elif line.startswith("DA:"):
            # Format: DA:<line_number>,<execution_count>[,<checksum>]
            parts = line[3:].split(",")
            if len(parts) >= 2:
                lines_found += 1
                try:
                    if int(parts[1]) > 0:
                        lines_hit += 1
                except ValueError:
                    pass
        elif line.startswith("LF:"):
            try:
                lines_found = int(line[3:])
            except ValueError:
                pass
        elif line.startswith("LH:"):
            try:
                lines_hit = int(line[3:])
            except ValueError:
                pass
        elif line.startswith("FNF:"):
            try:
                functions_found = int(line[4:])
            except ValueError:
                pass
        elif line.startswith("FNH:"):
            try:
                functions_hit = int(line[4:])
            except ValueError:
                pass
        elif line == "end_of_record":
            if current_file:
                files_stats[current_file] = {
                    "lines_found": lines_found,
                    "lines_hit": lines_hit,
                    "functions_found": functions_found,
                    "functions_hit": functions_hit,
                }
            current_file = None

    return files_stats


def compute_metrics(files_stats: Dict[str, Dict[str, int]], chip_root: Optional[Path] = None) -> Dict:
    """
    Aggregates file stats into overall and per-subsystem metrics.
    """
    total_lf = 0
    total_lh = 0
    total_fnf = 0
    total_fnh = 0

    subsystem_aggregates = {
        name: {"lines_found": 0, "lines_hit": 0, "functions_found": 0, "functions_hit": 0, "file_count": 0}
        for name, _ in SUBSYSTEMS
    }

    chip_root_str = str(chip_root.resolve()) if chip_root else ""

    for file_path, stats in files_stats.items():
        total_lf += stats["lines_found"]
        total_lh += stats["lines_hit"]
        total_fnf += stats["functions_found"]
        total_fnh += stats["functions_hit"]

        # Normalize relative to repo root if present
        rel_path = file_path
        if chip_root_str and rel_path.startswith(chip_root_str):
            rel_path = os.path.relpath(rel_path, chip_root_str)

        for name, prefix in SUBSYSTEMS:
            if rel_path.startswith(prefix) or f"/{prefix}" in rel_path:
                subsystem_aggregates[name]["lines_found"] += stats["lines_found"]
                subsystem_aggregates[name]["lines_hit"] += stats["lines_hit"]
                subsystem_aggregates[name]["functions_found"] += stats["functions_found"]
                subsystem_aggregates[name]["functions_hit"] += stats["functions_hit"]
                subsystem_aggregates[name]["file_count"] += 1
                break

    line_pct = (total_lh / total_lf * 100.0) if total_lf > 0 else 0.0
    func_pct = (total_fnh / total_fnf * 100.0) if total_fnf > 0 else 0.0

    subsystems_output = {}
    for name, _ in SUBSYSTEMS:
        s = subsystem_aggregates[name]
        s_lf = s["lines_found"]
        s_lh = s["lines_hit"]
        s_fnf = s["functions_found"]
        s_fnh = s["functions_hit"]
        subsystems_output[name] = {
            "file_count": s["file_count"],
            "lines_found": s_lf,
            "lines_hit": s_lh,
            "line_coverage_pct": round((s_lh / s_lf * 100.0) if s_lf > 0 else 0.0, 2),
            "functions_found": s_fnf,
            "functions_hit": s_fnh,
            "function_coverage_pct": round((s_fnh / s_fnf * 100.0) if s_fnf > 0 else 0.0, 2),
        }

    return {
        "overall": {
            "total_files": len(files_stats),
            "lines_found": total_lf,
            "lines_hit": total_lh,
            "line_coverage_pct": round(line_pct, 2),
            "functions_found": total_fnf,
            "functions_hit": total_fnh,
            "function_coverage_pct": round(func_pct, 2),
        },
        "subsystems": subsystems_output,
    }


def generate_markdown_summary(summary_data: Dict, title: str = "Matter Virtual-Home Coverage Summary") -> str:
    overall = summary_data["overall"]
    subsystems = summary_data["subsystems"]

    lines = [
        f"# {title}",
        "",
        "## Overall Coverage",
        "",
        "| Metric | Total | Covered | Coverage % |",
        "| :--- | :--- | :--- | :--- |",
        f"| **Lines** | {overall['lines_found']:,} | {overall['lines_hit']:,} | **{overall['line_coverage_pct']:.2f}%** |",
        f"| **Functions** | {overall['functions_found']:,} | {overall['functions_hit']:,} | **{overall['function_coverage_pct']:.2f}%** |",
        f"| **Instrumented Files** | {overall['total_files']:,} | - | - |",
        "",
        "## Subsystem Breakdown",
        "",
        "| Subsystem | Files | Lines Total | Lines Hit | Line % | Funcs Total | Funcs Hit | Func % |",
        "| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |",
    ]

    for name, _ in SUBSYSTEMS:
        s = subsystems.get(name, {})
        lf = s.get("lines_found", 0)
        lh = s.get("lines_hit", 0)
        lp = s.get("line_coverage_pct", 0.0)
        fnf = s.get("functions_found", 0)
        fnh = s.get("functions_hit", 0)
        fp = s.get("function_coverage_pct", 0.0)
        files = s.get("file_count", 0)
        lines.append(f"| `{name}` | {files} | {lf:,} | {lh:,} | {lp:.2f}% | {fnf:,} | {fnh:,} | {fp:.2f}% |")

    lines.append("")
    return "\n".join(lines)


def run_command(
    cmd: List[str], check: bool = True, capture_output: bool = True,
    timeout: Optional[int] = None
) -> subprocess.CompletedProcess:
    logger.info("Executing: %s", " ".join(cmd))
    res = subprocess.run(
        cmd,
        check=check,
        stdout=subprocess.PIPE if capture_output else None,
        stderr=subprocess.PIPE if capture_output else None,
        text=True,
        timeout=timeout,
    )
    return res


# lcov and genhtml exit before doing any work when --ignore-errors names a
# category they do not know. Categories differ across lcov versions: lcov 2.0
# (the chip-build-cirque CI image) rejects 'range', which lcov 2.4 accepts.
_UNKNOWN_IGNORE_ERROR_RE = re.compile(
    r"unknown argument for --ignore-errors:\s*'?([\w-]+)'?")


def run_with_supported_ignore_errors(
    base_cmd: List[str], ignore_errors: List[str], check: bool = True,
    timeout: Optional[int] = None
) -> subprocess.CompletedProcess:
    """Runs an lcov/genhtml command, dropping rejected --ignore-errors values.

    Retries without each category the installed tool reports as unknown, so a
    single category list works across lcov versions.
    """
    categories = list(ignore_errors)
    while True:
        cmd = list(base_cmd)
        if categories:
            cmd += ["--ignore-errors", ",".join(categories)]
        res = run_command(cmd, check=False, timeout=timeout)
        if res.returncode != 0:
            match = _UNKNOWN_IGNORE_ERROR_RE.search(res.stderr or "")
            if match and match.group(1) in categories:
                logger.warning(
                    "%s does not support --ignore-errors category '%s'; "
                    "retrying without it", base_cmd[0], match.group(1))
                categories.remove(match.group(1))
                continue
            if check:
                raise subprocess.CalledProcessError(
                    res.returncode, cmd, output=res.stdout, stderr=res.stderr)
        return res


def detect_gcov_tool(out_dir: Path) -> str:
    """Auto-detect gcov tool matching the version of .gcno files in out_dir."""
    env_gcov = os.environ.get("CIRQUE_GCOV_TOOL")
    if env_gcov:
        found = shutil.which(env_gcov)
        if found:
            return found

    target_major = None
    for gcno_file in out_dir.rglob("*.gcno"):
        try:
            with open(gcno_file, "rb") as f:
                magic = f.read(4)
                ver_bytes = f.read(4)
                # GCC gcno magic is b"oncg" (little-endian)
                # ver_bytes in little-endian: e.g. b"*33B" -> "B33*" (GCC 13)
                if magic == b"oncg" and len(ver_bytes) == 4:
                    ver_str = ver_bytes[::-1].decode("ascii", errors="ignore")
                    if (
                        ver_str.startswith("B")
                        and len(ver_str) >= 2
                        and ver_str[1].isdigit()
                    ):
                        target_major = str(10 + int(ver_str[1]))
                        break
                    if (
                        ver_str.startswith("A")
                        and len(ver_str) >= 2
                        and ver_str[1].isdigit()
                    ):
                        target_major = str(int(ver_str[1]))
                        break
        except (OSError, UnicodeDecodeError) as e:
            logger.debug("Failed reading gcno file %s: %s", gcno_file, e)

    candidates = []
    if target_major:
        candidates.append(f"gcov-{target_major}")
    candidates.extend(["gcov-13", "gcov-14", "gcov-15", "gcov"])

    for candidate in candidates:
        found = shutil.which(candidate)
        if found:
            return found
    return "gcov"


def detect_lcov_parallel_jobs() -> int:
    """Returns the job count to pass to lcov/genhtml --parallel, or 0 if unused.

    lcov >= 2.0 supports `--parallel N`; older releases reject the flag, so the
    version is probed first. CIRQUE_COVERAGE_JOBS overrides the job count
    (0 or 1 disables parallel processing).
    """
    override = os.environ.get("CIRQUE_COVERAGE_JOBS", "").strip()
    try:
        out = subprocess.run(["lcov", "--version"], capture_output=True,
                             text=True, timeout=30, check=False).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    match = re.search(r"(\d+)\.(\d+)", out or "")
    if not match or int(match.group(1)) < 2:
        return 0
    if override:
        try:
            jobs = int(override)
        except ValueError:
            jobs = 0
    else:
        jobs = os.cpu_count() or 1
    return jobs if jobs > 1 else 0


def coverage_capture_timeout() -> int:
    """Returns the per-command lcov timeout in seconds (CIRQUE_COVERAGE_TIMEOUT)."""
    try:
        return max(60, int(os.environ.get("CIRQUE_COVERAGE_TIMEOUT", "1800")))
    except ValueError:
        return 1800


def canonicalize_lcov_symlinks(raw_info: Path, chip_root: Path) -> None:
    """Normalizes GN example symlink paths in lcov_raw.info back to chip_root.

    GN builds under `examples/<app>/linux` reference the repository root via a
    `third_party/connectedhomeip` symlink. Canonicalizing `SF:` records before
    `lcov --remove` merges duplicate translation units and ensures
    `third_party/*` (e.g. perfetto, jsoncpp, nlio) is matched and excluded.
    """
    if not raw_info.exists():
        return
    chip_root_str = str(chip_root.resolve())
    symlink_re = re.compile(
        r"^SF:"
        + re.escape(chip_root_str)
        + r"/examples/[^/]+/[^/]+/third_party/connectedhomeip/"
    )
    content = raw_info.read_text(errors="replace")
    updated_lines = []
    changed = False
    for line in content.splitlines():
        if line.startswith("SF:"):
            new_line = symlink_re.sub(f"SF:{chip_root_str}/", line)
            if new_line != line:
                changed = True
            updated_lines.append(new_line)
        else:
            updated_lines.append(line)
    if changed:
        raw_info.write_text("\n".join(updated_lines) + "\n")


def _file_line_count(
    sf_path: str, cache: Dict[str, Optional[int]]
) -> Optional[int]:
    if sf_path in cache:
        return cache[sf_path]
    try:
        p = Path(sf_path)
        cache[sf_path] = (
            len(p.read_bytes().splitlines()) if p.is_file() else None
        )
    except OSError:
        cache[sf_path] = None
    return cache[sf_path]


def sanitize_lcov_out_of_range_lines(info_path: Path) -> int:
    """Drops DA/BRDA/FN records exceeding the source file's physical line count.

    Clang's `llvm-cov export -format=lcov` can emit an EOF counter at
    `line_count + 1` (e.g. `DA:85,...` on 84-line `StatusCodeList.h`), which
    causes `genhtml` parallel workers to abort with a `(range)` error before
    writing `html/index.html`.
    """
    if not info_path.exists():
        return 0
    line_count_cache: Dict[str, Optional[int]] = {}
    content = info_path.read_text(errors="replace")
    out_lines: List[str] = []
    current_max_line: Optional[int] = None
    removed = 0
    block_removed = 0
    block_da_total = 0
    block_da_hit = 0

    for line in content.splitlines():
        stripped = line.strip()
        if stripped.startswith("SF:"):
            sf_path = stripped[3:].strip()
            current_max_line = _file_line_count(sf_path, line_count_cache)
            block_removed = 0
            block_da_total = 0
            block_da_hit = 0
            out_lines.append(line)
        elif current_max_line is not None and stripped.startswith("DA:"):
            parts = stripped[3:].split(",")
            try:
                lineno = int(parts[0])
                hit = int(parts[1]) if len(parts) > 1 else 0
            except ValueError:
                out_lines.append(line)
                continue
            if lineno > current_max_line or lineno < 1:
                removed += 1
                block_removed += 1
                continue
            block_da_total += 1
            if hit > 0:
                block_da_hit += 1
            out_lines.append(line)
        elif current_max_line is not None and (
            stripped.startswith("BRDA:") or stripped.startswith("FN:")
        ):
            prefix_len = 5 if stripped.startswith("BRDA:") else 3
            first_field = stripped[prefix_len:].split(",", 1)[0]
            try:
                lineno = int(first_field)
            except ValueError:
                out_lines.append(line)
                continue
            if lineno > current_max_line or lineno < 1:
                removed += 1
                block_removed += 1
                continue
            out_lines.append(line)
        elif stripped.startswith("LF:") and block_removed > 0:
            out_lines.append(f"LF:{block_da_total}")
        elif stripped.startswith("LH:") and block_removed > 0:
            out_lines.append(f"LH:{block_da_hit}")
        elif stripped == "end_of_record":
            current_max_line = None
            block_removed = 0
            out_lines.append(line)
        else:
            out_lines.append(line)

    if removed > 0:
        info_path.write_text("\n".join(out_lines) + "\n")
        logger.info("Sanitized %d out-of-range coverage line record(s) in %s",
                    removed, info_path)
    return removed


# clang-built ARM32 Silabs apps (scripts/build/gn_gen_cirque_silabs_arm32.sh
# with CIRQUE_COVERAGE=1) write LLVM .profraw files instead of gcov .gcda.
LLVM_COVERAGE_BINARIES = (
    "out/arm-silabs/chip-silabs-lighting-app",
    "out/arm-silabs-thread/chip-silabs-lit-icd-app",
    "out/arm-silabs-thread/chip-silabs-lock-app",
)


def has_llvm_coverage_mapping(binary: Path) -> bool:
    """Returns True if binary was built with -fcoverage-mapping."""
    try:
        with open(binary, "rb") as f, mmap.mmap(
                f.fileno(), 0, access=mmap.ACCESS_READ) as data:
            return data.find(b"__llvm_covmap") != -1
    except (OSError, ValueError):
        return False


def append_llvm_profraw_coverage(out_dir: Path, output_dir: Path,
                                 chip_root: Path, raw_info: Path) -> bool:
    """Merges out_dir/profraw/*.profraw and appends them to raw_info as lcov.

    Returns True if records were appended.
    """
    profraw_files = sorted((out_dir / "profraw").glob("*.profraw"))
    if not profraw_files:
        return False
    llvm_bin = chip_root / ".environment/cipd/packages/pigweed/bin"
    llvm_profdata = llvm_bin / "llvm-profdata"
    llvm_cov = llvm_bin / "llvm-cov"
    if not (llvm_profdata.exists() and llvm_cov.exists()):
        logger.warning("Skipping %d .profraw files: %s not found",
                       len(profraw_files), llvm_bin)
        return False
    binaries = [chip_root / b for b in LLVM_COVERAGE_BINARIES]
    binaries = [b for b in binaries if has_llvm_coverage_mapping(b)]
    if not binaries:
        logger.warning("Skipping %d .profraw files: no binary with LLVM "
                       "coverage mapping", len(profraw_files))
        return False
    profdata = output_dir / "llvm_merged.profdata"
    export_cmd = [str(llvm_cov), "export", "-format=lcov",
                  "-instr-profile", str(profdata)]
    for binary in binaries:
        export_cmd += ["-object", str(binary)]
    try:
        run_command([str(llvm_profdata), "merge", "-sparse",
                     *map(str, profraw_files), "-o", str(profdata)])
        lcov_out = run_command(export_cmd).stdout
    except subprocess.CalledProcessError as e:
        logger.warning("LLVM coverage export failed: %s\nStderr: %s",
                       e, e.stderr)
        return False
    with open(raw_info, "a") as f:
        f.write("\n" + lcov_out)
    logger.info("Appended LLVM coverage from %d .profraw files (%s)",
                len(profraw_files), ", ".join(b.name for b in binaries))
    return True


def generate_coverage_report(out_dir: Path, output_dir: Path, chip_root: Path,
                             skip_genhtml: bool = False) -> Tuple[str, str]:
    """
    Executes lcov capture on out_dir, excludes unwanted directories, generates html,
    and produces summary json/md files.
    """
    out_dir = out_dir.resolve()
    chip_root = chip_root.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    raw_info = output_dir / "lcov_raw.info"
    filtered_info = output_dir / "lcov.info"
    html_dir = output_dir / "html"

    lcov_ignore_errors = [
        "format", "unsupported", "inconsistent", "unused", "gcov", "negative",
        "version", "empty", "source", "count",
    ]

    # Auto-detect gcov tool version matching compiler
    gcov_tool = detect_gcov_tool(out_dir)
    jobs = detect_lcov_parallel_jobs()
    timeout = coverage_capture_timeout()
    parallel_args = ["--parallel", str(jobs)] if jobs else []
    logger.info("lcov parallel jobs: %s, per-command timeout: %ss",
                jobs or "disabled", timeout)

    # Step 1: Capture coverage data
    raw_info.unlink(missing_ok=True)
    capture_cmd = [
        "lcov", "--capture",
        "--gcov-tool", gcov_tool,
        "--directory", str(out_dir),
        "--output-file", str(raw_info),
        "--rc", "geninfo_unexecuted_blocks=1",
        *parallel_args,
    ]
    try:
        run_with_supported_ignore_errors(
            capture_cmd, lcov_ignore_errors, check=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        logger.error("lcov capture timed out after %ss: %s", timeout, e)
        raise
    except subprocess.CalledProcessError as e:
        logger.warning("lcov capture warning/error (continuing if info created): %s\nStderr: %s", e, e.stderr)

    gcov_captured = raw_info.exists() and raw_info.stat().st_size > 0
    if not gcov_captured:
        # A run may exercise only the clang ARM32 apps (no .gcda files).
        raw_info.write_text("")
    llvm_appended = append_llvm_profraw_coverage(
        out_dir, output_dir, chip_root, raw_info)
    if not gcov_captured and not llvm_appended:
        logger.error("lcov failed to generate %s", raw_info)
        raise RuntimeError(f"Failed to generate {raw_info}")

    canonicalize_lcov_symlinks(raw_info, chip_root)
    sanitize_lcov_out_of_range_lines(raw_info)

    # Build anchored exclude patterns using chip_root to prevent false stripping
    exclude_patterns = [
        "*/zzz_generated/*",
        "*/third_party/*",
        f"{chip_root}/third_party/*",
        f"{chip_root}/out/*",
        f"{out_dir}/*",
        "/usr/include/*",
        "/usr/lib/*",
        f"{chip_root}/src/app/util/mock/*",
        f"{chip_root}/src/*/tests/*",
        f"{chip_root}/src/*/testing/*",
    ]

    # Step 2: Remove excluded patterns
    remove_cmd = [
        "lcov", "--remove", str(raw_info),
        *exclude_patterns,
        "--output-file", str(filtered_info),
    ]
    try:
        run_with_supported_ignore_errors(
            remove_cmd, lcov_ignore_errors, check=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        logger.error("lcov remove timed out after %ss: %s", timeout, e)
        raise
    except subprocess.CalledProcessError as e:
        logger.warning("lcov remove warning: %s\nStderr: %s", e, e.stderr)
        # If remove fails, fallback to raw_info
        shutil.copyfile(raw_info, filtered_info)

    # Remove the large unfiltered intermediate tracefile (~31 MB) once
    # filtered_info has been written so CI log archives stay under Chrome's
    # 50 MB SafeBrowsing / Enterprise Connector zip inspection limit.
    if filtered_info.exists() and filtered_info.stat().st_size > 0:
        raw_info.unlink(missing_ok=True)

    if os.environ.get("CIRQUE_SKIP_GENHTML", "0") == "1":
        skip_genhtml = True

    # Step 3: Run genhtml
    if not skip_genhtml:
        # lcov 2.0 rejects 'range' (it is dropped there); out-of-range records
        # were already removed by sanitize_lcov_out_of_range_lines().
        genhtml_ignore_errors = [
            "inconsistent", "corrupt", "unsupported", "empty", "format",
            "range", "source", "count",
        ]
        base_genhtml_cmd = [
            "genhtml", str(filtered_info),
            "--output-directory", str(html_dir),
            "--rc", "max_message_count=1000",
            "--header-title", "Matter Cirque Virtual-Home Coverage",
        ]
        genhtml_cmd = [*base_genhtml_cmd, *parallel_args]
        try:
            res = run_with_supported_ignore_errors(
                genhtml_cmd, genhtml_ignore_errors, check=False,
                timeout=timeout)
            if (
                (res.returncode != 0 or not (html_dir / "index.html").is_file())
                and parallel_args
            ):
                logger.warning(
                    "genhtml with %s did not produce index.html "
                    "(returncode=%s); retrying without --parallel.\nStderr: %s",
                    " ".join(parallel_args),
                    res.returncode,
                    res.stderr,
                )
                res = run_with_supported_ignore_errors(
                    base_genhtml_cmd, genhtml_ignore_errors, check=False,
                    timeout=timeout)
            if not (html_dir / "index.html").is_file():
                logger.warning(
                    "genhtml did not produce %s (returncode=%s)\nStderr: %s",
                    html_dir / "index.html",
                    res.returncode,
                    res.stderr,
                )
        except Exception as e:
            logger.warning("genhtml failed: %s", e)

    # Step 4: Parse info and generate json & markdown
    info_content = filtered_info.read_text(errors="replace")
    file_stats = parse_lcov_info(info_content)
    metrics = compute_metrics(file_stats, chip_root=chip_root)

    total_files = metrics.get("overall", {}).get("total_files", 0)
    if total_files == 0:
        logger.warning(
            "::warning::Coverage report generated 0 instrumented files! "
            "Check path normalization, lcov exclude filters, and .gcda files "
            "in %s",
            out_dir,
        )

    json_path = output_dir / "coverage_summary.json"
    md_path = output_dir / "coverage_summary.md"

    json_path.write_text(json.dumps(metrics, indent=2))
    md_summary = generate_markdown_summary(metrics)
    md_path.write_text(md_summary)

    logger.info("Coverage summary generated at %s and %s", json_path, md_path)
    return str(json_path), str(md_path)


def main():
    logging.basicConfig(level=logging.INFO, format="[%(levelname)s] %(message)s")
    parser = argparse.ArgumentParser(description="Cirque Code Coverage Report Generator")
    parser.add_argument("--out-dir", required=True, type=Path, help="Directory containing .gcda/.gcno files")
    parser.add_argument("--output-dir", required=True, type=Path, help="Target directory for coverage artifacts")
    parser.add_argument("--chip-root", type=Path, default=Path(__file__).parents[3], help="CHIP repo root directory")
    parser.add_argument("--skip-genhtml", action="store_true", help="Skip genhtml execution")

    args = parser.parse_args()
    generate_coverage_report(args.out_dir, args.output_dir, args.chip_root, args.skip_genhtml)


if __name__ == "__main__":
    main()

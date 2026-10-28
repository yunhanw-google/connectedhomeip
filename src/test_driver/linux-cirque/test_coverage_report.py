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

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

_CUR_DIR = Path(__file__).resolve().parent
if str(_CUR_DIR) not in sys.path:
    sys.path.insert(0, str(_CUR_DIR))

from helper.coverage_report import (
    append_llvm_profraw_coverage,
    canonicalize_lcov_symlinks,
    parse_lcov_info,
    compute_metrics,
    generate_markdown_summary,
    sanitize_lcov_out_of_range_lines,
    SUBSYSTEMS,
)

SAMPLE_LCOV_DATA = """TN:
SF:/home/user/connectedhomeip/src/app/CommandSender.cpp
FN:10,CommandSender::Init
FN:25,CommandSender::SendCommand
FNDA:1,CommandSender::Init
FNDA:0,CommandSender::SendCommand
FNF:2
FNH:1
DA:10,1
DA:11,1
DA:12,1
DA:25,0
DA:26,0
LF:5
LH:3
end_of_record
SF:/home/user/connectedhomeip/src/transport/SessionManager.cpp
FN:15,SessionManager::Init
FNDA:1,SessionManager::Init
FNF:1
FNH:1
DA:15,2
DA:16,2
LF:2
LH:2
end_of_record
SF:/home/user/connectedhomeip/examples/platform/linux/AppMain.cpp
FN:50,main
FNDA:1,main
FNF:1
FNH:1
DA:50,1
DA:51,0
LF:2
LH:1
end_of_record
"""


class TestCoverageReport(unittest.TestCase):

    def test_parse_lcov_info(self):
        stats = parse_lcov_info(SAMPLE_LCOV_DATA)
        self.assertEqual(len(stats), 3)

        app_stats = stats["/home/user/connectedhomeip/src/app/CommandSender.cpp"]
        self.assertEqual(app_stats["lines_found"], 5)
        self.assertEqual(app_stats["lines_hit"], 3)
        self.assertEqual(app_stats["functions_found"], 2)
        self.assertEqual(app_stats["functions_hit"], 1)

        transport_stats = stats["/home/user/connectedhomeip/src/transport/SessionManager.cpp"]
        self.assertEqual(transport_stats["lines_found"], 2)
        self.assertEqual(transport_stats["lines_hit"], 2)

    def test_compute_metrics(self):
        stats = parse_lcov_info(SAMPLE_LCOV_DATA)
        chip_root = Path("/home/user/connectedhomeip")
        metrics = compute_metrics(stats, chip_root=chip_root)

        overall = metrics["overall"]
        self.assertEqual(overall["total_files"], 3)
        self.assertEqual(overall["lines_found"], 9)
        self.assertEqual(overall["lines_hit"], 6)
        self.assertAlmostEqual(overall["line_coverage_pct"], 66.67, places=2)
        self.assertEqual(overall["functions_found"], 4)
        self.assertEqual(overall["functions_hit"], 3)
        self.assertAlmostEqual(overall["function_coverage_pct"], 75.0, places=2)

        subsystems = metrics["subsystems"]
        self.assertIn("src/app", subsystems)
        self.assertEqual(subsystems["src/app"]["file_count"], 1)
        self.assertEqual(subsystems["src/app"]["lines_found"], 5)
        self.assertEqual(subsystems["src/app"]["lines_hit"], 3)
        self.assertAlmostEqual(subsystems["src/app"]["line_coverage_pct"], 60.0, places=2)

        self.assertIn("src/transport", subsystems)
        self.assertEqual(subsystems["src/transport"]["lines_found"], 2)
        self.assertEqual(subsystems["src/transport"]["lines_hit"], 2)
        self.assertAlmostEqual(subsystems["src/transport"]["line_coverage_pct"], 100.0, places=2)

        self.assertIn("examples", subsystems)
        self.assertEqual(subsystems["examples"]["lines_found"], 2)
        self.assertEqual(subsystems["examples"]["lines_hit"], 1)
        self.assertAlmostEqual(subsystems["examples"]["line_coverage_pct"], 50.0, places=2)

    def test_generate_markdown_summary(self):
        stats = parse_lcov_info(SAMPLE_LCOV_DATA)
        metrics = compute_metrics(stats, chip_root=Path("/home/user/connectedhomeip"))
        md = generate_markdown_summary(metrics, title="Test Report")

        self.assertIn("# Test Report", md)
        self.assertIn("66.67%", md)
        self.assertIn("`src/app`", md)
        self.assertIn("`src/transport`", md)
        self.assertIn("`examples`", md)

    def test_compute_metrics_relative_and_unnormalized(self):
        # Test with unnormalized paths (e.g. scripts/tests/../..)
        chip_root = Path("/home/user/connectedhomeip")
        unnormalized_stats = {
            "/home/user/connectedhomeip/scripts/tests/../../"
            "src/app/Server.cpp": {
                "lines_found": 10,
                "lines_hit": 5,
                "functions_found": 2,
                "functions_hit": 1,
            },
            "/home/user/connectedhomeip/src/transport/Session.cpp": {
                "lines_found": 8,
                "lines_hit": 4,
                "functions_found": 1,
                "functions_hit": 1,
            },
        }
        # Normalize keys as coverage_report would
        normalized_stats = {
            os.path.normpath(k): v for k, v in unnormalized_stats.items()
        }
        metrics = compute_metrics(normalized_stats, chip_root=chip_root)
        self.assertEqual(metrics["overall"]["total_files"], 2)
        self.assertEqual(metrics["subsystems"]["src/app"]["file_count"], 1)
        self.assertEqual(metrics["subsystems"]["src/app"]["lines_found"], 10)
        self.assertEqual(
            metrics["subsystems"]["src/transport"]["file_count"], 1
        )

    def test_zero_instrumented_files_warning(self):
        empty_stats = {}
        metrics = compute_metrics(
            empty_stats, chip_root=Path("/home/user/connectedhomeip")
        )
        self.assertEqual(metrics["overall"]["total_files"], 0)
        self.assertEqual(metrics["overall"]["lines_found"], 0)
        self.assertEqual(metrics["overall"]["line_coverage_pct"], 0.0)

    def test_detect_gcov_tool(self):
        from helper.coverage_report import detect_gcov_tool
        import tempfile
        with tempfile.TemporaryDirectory() as tmpdir:
            tmppath = Path(tmpdir)
            # Create a mock gcno file with GCC 13 header (oncg + *33B)
            gcno_file = tmppath / "test.gcno"
            with open(gcno_file, "wb") as f:
                f.write(b"oncg*33Bextra_data")
            tool = detect_gcov_tool(tmppath)
            self.assertTrue(tool.endswith("gcov-13") or tool.endswith("gcov"))

    def test_canonicalize_lcov_symlinks(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            chip_root = Path(tmpdir) / "connectedhomeip"
            chip_root.mkdir()
            raw_info = Path(tmpdir) / "lcov_raw.info"
            raw_info.write_text(
                "TN:\n"
                f"SF:{chip_root}/examples/all-clusters-app/linux/third_party/"
                "connectedhomeip/src/app/Server.cpp\n"
                "DA:10,1\n"
                "end_of_record\n"
                "TN:\n"
                f"SF:{chip_root}/examples/lighting-app/linux/third_party/"
                "connectedhomeip/third_party/perfetto/repo/sdk/perfetto.cc\n"
                "DA:20,1\n"
                "end_of_record\n"
            )
            canonicalize_lcov_symlinks(raw_info, chip_root)
            normalized = raw_info.read_text()
            resolved = chip_root.resolve()
            self.assertIn(f"SF:{resolved}/src/app/Server.cpp\n", normalized)
            self.assertIn(
                f"SF:{resolved}/third_party/perfetto/repo/sdk/perfetto.cc\n",
                normalized,
            )
            self.assertNotIn(
                "examples/all-clusters-app/linux/third_party/connectedhomeip",
                normalized,
            )

    def test_sanitize_lcov_out_of_range_lines(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            src_file = Path(tmpdir) / "StatusCodeList.h"
            # 3 physical lines; DA:4 (LLVM EOF counter) is out of range.
            src_file.write_text("line1\nline2\nline3\n")
            raw_info = Path(tmpdir) / "lcov_raw.info"
            raw_info.write_text(
                "TN:\n"
                f"SF:{src_file}\n"
                "DA:1,1\n"
                "DA:3,2\n"
                "DA:4,1\n"
                "LF:3\n"
                "LH:3\n"
                "end_of_record\n"
            )
            removed = sanitize_lcov_out_of_range_lines(raw_info)
            self.assertEqual(removed, 1)
            sanitized = raw_info.read_text()
            self.assertIn("DA:1,1\n", sanitized)
            self.assertIn("DA:3,2\n", sanitized)
            self.assertNotIn("DA:4,1", sanitized)
            self.assertIn("LF:2\n", sanitized)
            self.assertIn("LH:2\n", sanitized)


class TestAppendLlvmProfrawCoverage(unittest.TestCase):
    """Verifies ARM32 .profraw files are merged into the lcov tracefile."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.chip_root = root / "connectedhomeip"
        self.out_dir = self.chip_root / "out/debug"
        self.output_dir = root / "cov"
        self.output_dir.mkdir(parents=True)
        self.raw_info = self.output_dir / "lcov_raw.info"
        self.raw_info.write_text("SF:/x86/only.cpp\nend_of_record\n")
        bin_dir = self.chip_root / ".environment/cipd/packages/pigweed/bin"
        bin_dir.mkdir(parents=True)
        self.cov_args = root / "llvm-cov.args"
        # llvm-profdata writes the -o file; llvm-cov records its arguments
        # and prints one lcov record.
        (bin_dir / "llvm-profdata").write_text(
            '#!/bin/sh\nwhile [ "$1" != "-o" ]; do shift; done; '
            'echo merged > "$2"\n')
        (bin_dir / "llvm-cov").write_text(
            f'#!/bin/sh\necho "$@" > {self.cov_args}\n'
            'printf "SF:/arm/silabs/AppTask.cpp\\nDA:1,1\\n'
            'end_of_record\\n"\n')
        for tool in ("llvm-profdata", "llvm-cov"):
            (bin_dir / tool).chmod(0o755)

    def tearDown(self):
        self._tmp.cleanup()

    def _write(self, rel: str, content: bytes) -> Path:
        path = self.chip_root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def _append(self) -> bool:
        return append_llvm_profraw_coverage(
            self.out_dir, self.output_dir, self.chip_root, self.raw_info)

    def test_no_profraw_leaves_tracefile_unchanged(self):
        self._write("out/arm-silabs-thread/chip-silabs-lit-icd-app",
                    b"ELF __llvm_covmap")
        self.assertFalse(self._append())
        self.assertEqual(self.raw_info.read_text(),
                         "SF:/x86/only.cpp\nend_of_record\n")

    def test_profraw_without_instrumented_binary_is_skipped(self):
        self._write("out/debug/profraw/lit-icd-1-0.profraw", b"raw")
        self._write("out/arm-silabs-thread/chip-silabs-lit-icd-app", b"ELF")
        self.assertFalse(self._append())
        self.assertFalse(self.cov_args.exists())

    def test_profraw_is_exported_for_instrumented_binaries_only(self):
        self._write("out/debug/profraw/lit-icd-1-0.profraw", b"raw")
        lit_icd = self._write(
            "out/arm-silabs-thread/chip-silabs-lit-icd-app",
            b"ELF __llvm_covmap")
        lighting = self._write("out/arm-silabs/chip-silabs-lighting-app",
                               b"ELF")
        self.assertTrue(self._append())
        info = self.raw_info.read_text()
        self.assertIn("SF:/x86/only.cpp\n", info)
        self.assertIn("SF:/arm/silabs/AppTask.cpp\n", info)
        args = self.cov_args.read_text()
        self.assertIn(f"-object {lit_icd}", args)
        self.assertNotIn(str(lighting), args)

    @unittest.skipUnless(shutil.which("lcov"), "lcov not installed")
    def test_report_from_profraw_only_run(self):
        """A run with no .gcda files still reports the ARM32 apps."""
        from helper.coverage_report import generate_coverage_report
        self._write("out/debug/profraw/lit-icd-1-0.profraw", b"raw")
        self._write("out/arm-silabs-thread/chip-silabs-lit-icd-app",
                    b"ELF __llvm_covmap")
        # setUp left a stale lcov_raw.info; it must not leak into the report.
        json_path, _ = generate_coverage_report(
            self.out_dir, self.output_dir, self.chip_root, skip_genhtml=True)
        metrics = json.loads(Path(json_path).read_text())
        self.assertEqual(metrics["overall"]["total_files"], 1)
        info = (self.output_dir / "lcov.info").read_text()
        self.assertIn("SF:/arm/silabs/AppTask.cpp", info)
        self.assertNotIn("/x86/only.cpp", info)


def _write_fake_lcov(bin_dir: Path, version_line: str) -> None:
    """Writes an `lcov` executable that only answers --version."""
    script = bin_dir / "lcov"
    script.write_text(f"#!/bin/sh\necho '{version_line}'\n")
    script.chmod(0o755)


# Shell snippet that exits like lcov/genhtml 2.0 (chip-build-cirque CI image)
# when --ignore-errors names 'range', which that version does not know.
_REJECT_RANGE_SH = (
    'prev=""\n'
    'for a in "$@"; do\n'
    '  if [ "$prev" = "--ignore-errors" ]; then\n'
    '    case ",$a," in *,range,*)\n'
    "      echo \"$(basename \"$0\"): ERROR: unknown argument for"
    " --ignore-errors: 'range'\" >&2\n"
    '      exit 255;;\n'
    '    esac\n'
    '  fi\n'
    '  prev="$a"\n'
    'done\n'
)


class TestRunWithSupportedIgnoreErrors(unittest.TestCase):
    """Verifies rejected --ignore-errors categories are dropped and retried."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.calls = self.root / "calls.txt"

    def tearDown(self):
        self._tmp.cleanup()

    def _tool(self, body: str) -> str:
        tool = self.root / "genhtml"
        tool.write_text(
            "#!/bin/sh\n"
            f'echo "$@" >> {self.calls}\n' + body)
        tool.chmod(0o755)
        return str(tool)

    def test_drops_category_rejected_by_lcov_2_0(self):
        from helper.coverage_report import run_with_supported_ignore_errors
        tool = self._tool(_REJECT_RANGE_SH + "exit 0\n")
        res = run_with_supported_ignore_errors(
            [tool, "lcov.info"], ["inconsistent", "range", "source"])
        self.assertEqual(res.returncode, 0)
        calls = self.calls.read_text().splitlines()
        self.assertEqual(len(calls), 2)
        self.assertIn("--ignore-errors inconsistent,range,source", calls[0])
        self.assertEqual(
            calls[1], "lcov.info --ignore-errors inconsistent,source")

    def test_other_failures_are_not_retried(self):
        from helper.coverage_report import run_with_supported_ignore_errors
        tool = self._tool(
            "echo 'genhtml: ERROR: no valid records' >&2\nexit 1\n")
        with self.assertRaises(subprocess.CalledProcessError):
            run_with_supported_ignore_errors([tool, "lcov.info"], ["range"])
        res = run_with_supported_ignore_errors(
            [tool, "lcov.info"], ["range"], check=False)
        self.assertEqual(res.returncode, 1)
        self.assertEqual(len(self.calls.read_text().splitlines()), 2)


class TestLcovParallelDetection(unittest.TestCase):
    """Verifies lcov --parallel feature detection and overrides."""

    def setUp(self):
        self._saved_env = {k: os.environ.get(k) for k in ("PATH", "CIRQUE_COVERAGE_JOBS")}
        self._tmp = tempfile.TemporaryDirectory()
        self.bin_dir = Path(self._tmp.name)
        os.environ["PATH"] = f"{self.bin_dir}{os.pathsep}{self._saved_env['PATH'] or ''}"
        os.environ.pop("CIRQUE_COVERAGE_JOBS", None)

    def tearDown(self):
        for key, value in self._saved_env.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        self._tmp.cleanup()

    def test_lcov_1x_disables_parallel(self):
        from helper.coverage_report import detect_lcov_parallel_jobs
        _write_fake_lcov(self.bin_dir, "lcov: LCOV version 1.16")
        self.assertEqual(detect_lcov_parallel_jobs(), 0)

    def test_lcov_2x_enables_parallel_with_cpu_count(self):
        from helper.coverage_report import detect_lcov_parallel_jobs
        _write_fake_lcov(self.bin_dir, "lcov: LCOV version 2.0-1")
        expected = os.cpu_count() or 1
        self.assertEqual(detect_lcov_parallel_jobs(), expected if expected > 1 else 0)

    def test_override_jobs(self):
        from helper.coverage_report import detect_lcov_parallel_jobs
        _write_fake_lcov(self.bin_dir, "lcov: LCOV version 2.0-1")
        os.environ["CIRQUE_COVERAGE_JOBS"] = "4"
        self.assertEqual(detect_lcov_parallel_jobs(), 4)
        os.environ["CIRQUE_COVERAGE_JOBS"] = "1"
        self.assertEqual(detect_lcov_parallel_jobs(), 0)

    def test_override_ignored_on_lcov_1x(self):
        from helper.coverage_report import detect_lcov_parallel_jobs
        _write_fake_lcov(self.bin_dir, "lcov: LCOV version 1.14")
        os.environ["CIRQUE_COVERAGE_JOBS"] = "8"
        self.assertEqual(detect_lcov_parallel_jobs(), 0)


@unittest.skipUnless(shutil.which("lcov") and shutil.which("gcc") and shutil.which("gcov"),
                     "requires real lcov, gcc and gcov")
class TestGenerateCoverageReportRealLcov(unittest.TestCase):
    """Runs the real lcov pipeline on a tiny instrumented C program."""

    def test_real_capture_produces_tracefile(self):
        from helper.coverage_report import generate_coverage_report
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            build = root / "out"
            build.mkdir()
            src = root / "src" / "app" / "hello.c"
            src.parent.mkdir(parents=True)
            src.write_text("int f(int x) { if (x) return 1; return 0; }\n"
                           "int main(void) { return f(0); }\n")
            exe = build / "hello"
            subprocess.run(
                ["gcc", "--coverage", "-O0", "-o", str(exe), str(src)],
                cwd=build,
                check=True,
            )
            subprocess.run([str(exe)], cwd=build, check=False)
            os.environ["CIRQUE_GCOV_TOOL"] = "gcov"
            try:
                json_path, _ = generate_coverage_report(
                    build, root / "cov", root, skip_genhtml=True)
            finally:
                os.environ.pop("CIRQUE_GCOV_TOOL", None)
            metrics = json.loads(Path(json_path).read_text())
            self.assertGreaterEqual(metrics["overall"]["total_files"], 1)
            self.assertGreater(metrics["overall"]["lines_hit"], 0)
            self.assertTrue((root / "cov" / "lcov.info").exists())
            self.assertFalse((root / "cov" / "lcov_raw.info").exists())

    def test_cirque_skip_genhtml_env_var(self):
        from helper.coverage_report import generate_coverage_report
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            build = root / "out"
            build.mkdir()
            src = root / "src" / "app" / "hello.c"
            src.parent.mkdir(parents=True)
            src.write_text("int f(int x) { return x + 1; }\n"
                           "int main(void) { return f(1); }\n")
            exe = build / "hello"
            subprocess.run(
                ["gcc", "--coverage", "-O0", "-o", str(exe), str(src)],
                cwd=build,
                check=True,
            )
            subprocess.run([str(exe)], cwd=build, check=False)
            os.environ["CIRQUE_GCOV_TOOL"] = "gcov"
            os.environ["CIRQUE_SKIP_GENHTML"] = "1"
            try:
                generate_coverage_report(
                    build, root / "cov", root, skip_genhtml=False)
            finally:
                os.environ.pop("CIRQUE_GCOV_TOOL", None)
                os.environ.pop("CIRQUE_SKIP_GENHTML", None)
            self.assertFalse((root / "cov" / "html").exists())
            self.assertFalse((root / "cov" / "lcov_raw.info").exists())

    @unittest.skipUnless(shutil.which("genhtml"), "requires genhtml")
    def test_genhtml_produces_index_html_with_out_of_range_llvm_line(self):
        """Regression test: llvm-cov DA:(N+1) on an N-line header must not
        abort genhtml before writing html/index.html."""
        self._assert_genhtml_writes_index_html()

    @unittest.skipUnless(shutil.which("genhtml"), "requires genhtml")
    def test_genhtml_writes_index_html_when_lcov_rejects_range(self):
        """Regression test: the CI image ships lcov 2.0, whose genhtml exits
        255 on '--ignore-errors range'; html/index.html must still be written.
        """
        real_genhtml = shutil.which("genhtml")
        with tempfile.TemporaryDirectory() as bin_tmp:
            wrapper = Path(bin_tmp) / "genhtml"
            wrapper.write_text(
                "#!/bin/sh\n" + _REJECT_RANGE_SH
                + f'exec {real_genhtml} "$@"\n')
            wrapper.chmod(0o755)
            saved_path = os.environ["PATH"]
            os.environ["PATH"] = f"{bin_tmp}{os.pathsep}{saved_path}"
            try:
                self._assert_genhtml_writes_index_html()
            finally:
                os.environ["PATH"] = saved_path

    def _assert_genhtml_writes_index_html(self):
        from helper.coverage_report import generate_coverage_report
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            out_dir = root / "out" / "debug"
            (out_dir / "profraw").mkdir(parents=True)
            (out_dir / "profraw" / "lit-icd-1.profraw").write_bytes(b"raw")
            bin_path = root / "out/arm-silabs-thread/chip-silabs-lit-icd-app"
            bin_path.parent.mkdir(parents=True)
            bin_path.write_bytes(b"ELF __llvm_covmap")
            hdr = root / "src/protocols/interaction_model/StatusCodeList.h"
            hdr.parent.mkdir(parents=True)
            hdr.write_text("// line 1\n// line 2\n")

            llvm_bin = root / ".environment/cipd/packages/pigweed/bin"
            llvm_bin.mkdir(parents=True)
            (llvm_bin / "llvm-profdata").write_text(
                '#!/bin/sh\nwhile [ "$1" != "-o" ]; do shift; done; '
                'echo merged > "$2"\n'
            )
            # Emit DA:3 on a 2-line file (reproducing StatusCodeList.h DA:85).
            (llvm_bin / "llvm-cov").write_text(
                "#!/bin/sh\n"
                f'printf "SF:{hdr}\\nDA:1,1\\nDA:2,1\\nDA:3,1\\n'
                'LF:3\\nLH:3\\nend_of_record\\n"\n'
            )
            for tool in ("llvm-profdata", "llvm-cov"):
                (llvm_bin / tool).chmod(0o755)

            cov_dir = root / "cov"
            generate_coverage_report(out_dir, cov_dir, root, skip_genhtml=False)
            self.assertTrue(
                (cov_dir / "html" / "index.html").is_file(),
                "Expected html/index.html to be generated by genhtml",
            )


if __name__ == "__main__":
    unittest.main()

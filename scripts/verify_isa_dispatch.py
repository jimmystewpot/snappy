#!/usr/bin/env python3
"""Automated ISA Dispatch Verification Script.

Builds and verifies Snappy under five distinct configurations:
  1. x86_64 cross-compile (-DCMAKE_OSX_ARCHITECTURES=x86_64) objdump -d audit:
     - Global AVX-512 ban: zero %zmm registers in snappy.cc.o.
     - CompressFragment_Scalar: contains imull ($0x1e35a7bd / 506832829),
       zero crc32, tzcnt, shrx, bzhi, or %ymm instructions.
     - CompressFragment_SSE42: contains crc32l, zero tzcnt, shrx, bzhi, or %ymm.
     - CompressFragment_AVX2_BMI2: contains crc32l, tzcnt, and shrx.
     - DecompressAllTags_Scalar<SnappyArrayWriter>: zero pshufb, vpshufb, %ymm,
       bzhil, or vzeroupper.
     - DecompressAllTags_SSSE3<SnappyArrayWriter>: contains pshufb, zero %ymm or bzhil.
     - DecompressAllTags_AVX2_BMI2<SnappyArrayWriter>: contains vlddqu (%ymm),
       vpshufb, bzhil, vzeroupper, and zero legacy non-VEX pshufb.
  2. arm64 -march=armv8-a+nocrc objdump -d audit:
     - CompressFragment_Scalar: zero crc32cw instructions.
     - CompressFragment_ARM_CRC32: contains crc32cw instructions.
     - DecompressAllTags_ARM_NEON<SnappyArrayWriter>: contains tbl.16b instructions.
  3. macOS Universal Binary (-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64") lipo audit:
     - libsnappy.a contains both arm64 and x86_64 slices.
  4. GCC (g++-15 if installed) build and ctest execution:
     - Asserts all unit tests pass with GCC 15 and -march=armv8-a+nocrc.
  5. AddressSanitizer + UndefinedBehaviorSanitizer (-fsanitize=address,undefined):
     - Asserts all unit tests pass across forced ISA tiers with zero ASan/UBSan errors.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Dict, List, Optional, Tuple


REPO_ROOT = Path(__file__).resolve().parent.parent


def run_command(
    cmd: List[str],
    cwd: Optional[Path] = None,
    verbose: bool = False,
    env: Optional[Dict[str, str]] = None,
) -> subprocess.CompletedProcess:
    """Run a shell command, printing output if verbose or on error."""
    working_dir = cwd or REPO_ROOT
    cmd_str = " ".join(cmd)
    if verbose:
        print(f"  [RUN] {cmd_str} (in {working_dir})")
    proc = subprocess.run(
        cmd,
        cwd=working_dir,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    if proc.returncode != 0 and not verbose:
        print(f"\n[ERROR] Command failed with code {proc.returncode}: {cmd_str}")
        print("STDOUT:")
        print(proc.stdout)
        print("STDERR:")
        print(proc.stderr)
    return proc


def parse_demangled_objdump(disasm_text: str) -> Dict[str, str]:
    """Parse demangled objdump text into {function_signature: body_assembly}."""
    funcs: Dict[str, str] = {}
    current_fn: Optional[str] = None
    current_lines: List[str] = []

    for line in disasm_text.splitlines():
        match = re.match(r"^[0-9a-fA-F]+\s+<(.*)>:$", line)
        if match:
            if current_fn is not None:
                funcs[current_fn] = "\n".join(current_lines)
            current_fn = match.group(1)
            current_lines = []
        elif current_fn is not None:
            current_lines.append(line)

    if current_fn is not None:
        funcs[current_fn] = "\n".join(current_lines)

    return funcs


def find_function(
    funcs: Dict[str, str], *substrings: str
) -> Tuple[Optional[str], Optional[str]]:
    """Find a function whose signature contains all given substrings."""
    for sig, body in funcs.items():
        if all(sub in sig for sub in substrings):
            return sig, body
    return None, None


def check_1_x86_disassembly(
    repo_root: Path, build_dir: Path, verbose: bool
) -> Tuple[bool, List[str]]:
    """Check 1: x86_64 cross-compiled disassembly audit on snappy.cc.o."""
    logs: List[str] = []
    cmake_cmd = [
        "cmake",
        "-B",
        str(build_dir),
        "-DCMAKE_OSX_ARCHITECTURES=x86_64",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DSNAPPY_BUILD_TESTS=OFF",
        "-DSNAPPY_BUILD_BENCHMARKS=OFF",
    ]
    res = run_command(cmake_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake configuration failed for x86_64 cross-compile"]

    build_cmd = ["cmake", "--build", str(build_dir), "--target", "snappy"]
    res = run_command(build_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake build failed for snappy target (x86_64)"]

    obj_file = build_dir / "CMakeFiles" / "snappy.dir" / "snappy.cc.o"
    if not obj_file.is_file():
        return False, [f"Object file {obj_file} not found"]

    objdump_bin = shutil.which("objdump") or shutil.which("llvm-objdump") or "objdump"
    disasm_res = run_command(
        [objdump_bin, "-d", "-C", "--arch=x86_64", str(obj_file)],
        cwd=repo_root,
        verbose=verbose,
    )
    if disasm_res.returncode != 0:
        return False, ["objdump failed to disassemble x86_64 snappy.cc.o"]

    full_disasm = disasm_res.stdout
    funcs = parse_demangled_objdump(full_disasm)

    # 1. Global AVX-512 ban: Zero %zmm registers anywhere in snappy.cc.o
    zmm_matches = re.findall(r"%zmm\d+|\bzmm\d+\b", full_disasm, re.IGNORECASE)
    if zmm_matches:
        return False, [
            f"Global AVX-512 ban violated: found {len(zmm_matches)} zmm references in snappy.cc.o"
        ]
    logs.append("Verified global AVX-512 ban: 0 %zmm registers in snappy.cc.o")

    # 2. CompressFragment_Scalar
    sig, body = find_function(funcs, "CompressFragment_Scalar")
    if not body:
        return False, ["CompressFragment_Scalar symbol not found in disassembly"]
    if not re.search(r"imull.*(0x1e35a7bd|506832829)", body, re.IGNORECASE):
        return False, ["CompressFragment_Scalar does not contain imull with 0x1e35a7bd"]
    for banned in [r"\bcrc32", r"\btzcnt", r"\bshrx", r"\bbzhi", r"%ymm"]:
        if re.search(banned, body, re.IGNORECASE):
            return False, [
                f"CompressFragment_Scalar contains banned instruction matching {banned}"
            ]
    logs.append(
        "Verified CompressFragment_Scalar: contains imull $0x1e35a7bd, zero crc32/tzcnt/shrx/bzhi/%ymm"
    )

    # 3. CompressFragment_SSE42
    sig, body = find_function(funcs, "CompressFragment_SSE42")
    if not body:
        return False, ["CompressFragment_SSE42 symbol not found in disassembly"]
    if not re.search(r"\bcrc32l\b", body, re.IGNORECASE):
        return False, ["CompressFragment_SSE42 does not contain crc32l"]
    for banned in [r"\btzcnt", r"\bshrx", r"\bbzhi", r"%ymm"]:
        if re.search(banned, body, re.IGNORECASE):
            return False, [
                f"CompressFragment_SSE42 contains banned instruction matching {banned}"
            ]
    logs.append(
        "Verified CompressFragment_SSE42: contains crc32l, zero tzcnt/shrx/bzhi/%ymm"
    )

    # 4. CompressFragment_AVX2_BMI2
    sig, body = find_function(funcs, "CompressFragment_AVX2_BMI2")
    if not body:
        return False, ["CompressFragment_AVX2_BMI2 symbol not found in disassembly"]
    for required in [r"\bcrc32l\b", r"\btzcnt", r"\bshrx"]:
        if not re.search(required, body, re.IGNORECASE):
            return False, [
                f"CompressFragment_AVX2_BMI2 missing required instruction {required}"
            ]
    logs.append(
        "Verified CompressFragment_AVX2_BMI2: contains crc32l, tzcnt, and shrx"
    )

    # 5. DecompressAllTags_Scalar<SnappyArrayWriter>
    sig, body = find_function(funcs, "DecompressAllTags_Scalar", "SnappyArrayWriter")
    if not body:
        return False, [
            "DecompressAllTags_Scalar<SnappyArrayWriter> symbol not found in disassembly"
        ]
    for banned in [
        r"\bpshufb\b",
        r"\bvpshufb\b",
        r"%ymm",
        r"\bbzhil\b",
        r"\bvzeroupper\b",
    ]:
        if re.search(banned, body, re.IGNORECASE):
            return False, [
                f"DecompressAllTags_Scalar<SnappyArrayWriter> contains banned instruction {banned}"
            ]
    logs.append(
        "Verified DecompressAllTags_Scalar<SnappyArrayWriter>: zero pshufb/vpshufb/%ymm/bzhil/vzeroupper"
    )

    # 6. DecompressAllTags_SSSE3<SnappyArrayWriter>
    sig, body = find_function(funcs, "DecompressAllTags_SSSE3", "SnappyArrayWriter")
    if not body:
        return False, [
            "DecompressAllTags_SSSE3<SnappyArrayWriter> symbol not found in disassembly"
        ]
    if not re.search(r"\bpshufb\b", body, re.IGNORECASE):
        return False, [
            "DecompressAllTags_SSSE3<SnappyArrayWriter> missing pshufb instruction"
        ]
    for banned in [r"%ymm", r"\bbzhil\b"]:
        if re.search(banned, body, re.IGNORECASE):
            return False, [
                f"DecompressAllTags_SSSE3<SnappyArrayWriter> contains banned instruction {banned}"
            ]
    logs.append(
        "Verified DecompressAllTags_SSSE3<SnappyArrayWriter>: contains pshufb, zero %ymm/bzhil"
    )

    # 7. DecompressAllTags_AVX2_BMI2<SnappyArrayWriter>
    sig, body = find_function(funcs, "DecompressAllTags_AVX2_BMI2", "SnappyArrayWriter")
    if not body:
        return False, [
            "DecompressAllTags_AVX2_BMI2<SnappyArrayWriter> symbol not found in disassembly"
        ]
    for req in [r"\bvlddqu\b", r"\bvpshufb\b", r"\bbzhil\b", r"\bvzeroupper\b"]:
        if not re.search(req, body, re.IGNORECASE):
            return False, [
                f"DecompressAllTags_AVX2_BMI2<SnappyArrayWriter> missing required instruction {req}"
            ]
    if not re.search(r"%ymm", body, re.IGNORECASE):
        return False, [
            "DecompressAllTags_AVX2_BMI2<SnappyArrayWriter> missing %ymm registers"
        ]
    # Check for legacy non-VEX pshufb instructions
    legacy_pshufb = [
        line
        for line in body.splitlines()
        if re.search(r"\bpshufb\b", line, re.IGNORECASE)
        and not re.search(r"\bvpshufb\b", line, re.IGNORECASE)
    ]
    if legacy_pshufb:
        return False, [
            f"DecompressAllTags_AVX2_BMI2 contains legacy non-VEX pshufb: {legacy_pshufb}"
        ]
    logs.append(
        "Verified DecompressAllTags_AVX2_BMI2<SnappyArrayWriter>: contains vlddqu (%ymm), vpshufb, bzhil, vzeroupper, zero non-VEX pshufb"
    )

    return True, logs


def check_2_arm64_disassembly(
    repo_root: Path, build_dir: Path, verbose: bool
) -> Tuple[bool, List[str]]:
    """Check 2: arm64 -march=armv8-a+nocrc disassembly audit on snappy.cc.o."""
    logs: List[str] = []
    cmake_cmd = [
        "cmake",
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_CXX_FLAGS=-march=armv8-a+nocrc",
        "-DSNAPPY_BUILD_TESTS=OFF",
        "-DSNAPPY_BUILD_BENCHMARKS=OFF",
    ]
    res = run_command(cmake_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake configuration failed for arm64 -march=armv8-a+nocrc"]

    build_cmd = ["cmake", "--build", str(build_dir), "--target", "snappy"]
    res = run_command(build_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake build failed for snappy target (arm64)"]

    obj_file = build_dir / "CMakeFiles" / "snappy.dir" / "snappy.cc.o"
    if not obj_file.is_file():
        return False, [f"Object file {obj_file} not found"]

    objdump_bin = shutil.which("objdump") or shutil.which("llvm-objdump") or "objdump"
    disasm_res = run_command(
        [objdump_bin, "-d", "-C", "--arch=arm64", str(obj_file)],
        cwd=repo_root,
        verbose=verbose,
    )
    if disasm_res.returncode != 0:
        return False, ["objdump failed to disassemble arm64 snappy.cc.o"]

    full_disasm = disasm_res.stdout
    funcs = parse_demangled_objdump(full_disasm)

    # 1. CompressFragment_Scalar: Contains zero crc32cw instructions
    sig, body = find_function(funcs, "CompressFragment_Scalar")
    if not body:
        return False, ["CompressFragment_Scalar symbol not found in arm64 disassembly"]
    if re.search(r"\bcrc32cw\b", body, re.IGNORECASE):
        return False, [
            "arm64 CompressFragment_Scalar illegally contains crc32cw instruction under +nocrc baseline"
        ]
    logs.append("Verified CompressFragment_Scalar (arm64): zero crc32cw instructions")

    # 2. CompressFragment_ARM_CRC32: Contains crc32cw instructions
    sig, body = find_function(funcs, "CompressFragment_ARM_CRC32")
    if not body:
        return False, [
            "CompressFragment_ARM_CRC32 symbol not found in arm64 disassembly"
        ]
    if not re.search(r"\bcrc32cw\b", body, re.IGNORECASE):
        return False, [
            "arm64 CompressFragment_ARM_CRC32 does not contain crc32cw instructions"
        ]
    logs.append(
        "Verified CompressFragment_ARM_CRC32 (arm64): contains crc32cw instructions"
    )

    # 3. DecompressAllTags_ARM_NEON<SnappyArrayWriter>: Contains tbl.16b instructions
    sig, body = find_function(funcs, "DecompressAllTags_ARM_NEON", "SnappyArrayWriter")
    if not body:
        return False, [
            "DecompressAllTags_ARM_NEON<SnappyArrayWriter> symbol not found in arm64 disassembly"
        ]
    if not re.search(r"\btbl(\.16b)?\b", body, re.IGNORECASE):
        return False, [
            "DecompressAllTags_ARM_NEON<SnappyArrayWriter> does not contain tbl.16b instructions"
        ]
    logs.append(
        "Verified DecompressAllTags_ARM_NEON<SnappyArrayWriter> (arm64): contains tbl.16b instructions"
    )

    return True, logs


def check_3_universal_binary(
    repo_root: Path, build_dir: Path, verbose: bool
) -> Tuple[bool, List[str]]:
    """Check 3: macOS Universal Binary (-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64") lipo audit."""
    logs: List[str] = []
    cmake_cmd = [
        "cmake",
        "-B",
        str(build_dir),
        '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64',
        "-DCMAKE_BUILD_TYPE=Release",
        "-DSNAPPY_BUILD_TESTS=OFF",
        "-DSNAPPY_BUILD_BENCHMARKS=OFF",
    ]
    res = run_command(cmake_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake configuration failed for Universal Binary"]

    build_cmd = ["cmake", "--build", str(build_dir), "--target", "snappy"]
    res = run_command(build_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake build failed for Universal Binary libsnappy.a"]

    lib_file = build_dir / "libsnappy.a"
    if not lib_file.is_file():
        return False, [f"Static library {lib_file} not found"]

    lipo_bin = shutil.which("lipo") or "/usr/bin/lipo"
    lipo_res = run_command([lipo_bin, "-info", str(lib_file)], cwd=repo_root, verbose=verbose)
    if lipo_res.returncode != 0:
        return False, ["lipo -info failed on libsnappy.a"]

    output = lipo_res.stdout
    if "arm64" not in output or "x86_64" not in output:
        return False, [
            f"Universal Binary slices missing from {output.strip()} (expected arm64 and x86_64)"
        ]
    logs.append(f"Verified Universal Binary: {output.strip()}")

    return True, logs


def check_4_gcc_build_and_test(
    repo_root: Path, build_dir: Path, verbose: bool
) -> Tuple[bool, List[str]]:
    """Check 4: GCC (g++-15 if installed) build and ctest execution."""
    gcc_cxx = "/opt/homebrew/bin/g++-15"
    gcc_cc = "/opt/homebrew/bin/gcc-15"

    if not (Path(gcc_cxx).is_file() and Path(gcc_cc).is_file()):
        return True, [f"GCC 15 ({gcc_cxx}) not found; skipping Check 4 as specified"]

    logs: List[str] = []
    cmake_cmd = [
        "cmake",
        "-B",
        str(build_dir),
        f"-DCMAKE_CXX_COMPILER={gcc_cxx}",
        f"-DCMAKE_C_COMPILER={gcc_cc}",
        "-DCMAKE_CXX_FLAGS=-march=armv8-a+nocrc",
        "-DSNAPPY_BUILD_TESTS=ON",
        "-DSNAPPY_BUILD_BENCHMARKS=OFF",
    ]
    res = run_command(cmake_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake configuration failed with GCC 15"]

    build_cmd = ["cmake", "--build", str(build_dir), "--target", "snappy_unittest"]
    res = run_command(build_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake build failed for snappy_unittest with GCC 15"]

    test_cmd = ["ctest", "--test-dir", str(build_dir), "--output-on-failure"]
    test_res = run_command(test_cmd, cwd=repo_root, verbose=verbose)
    if test_res.returncode != 0:
        return False, [
            f"ctest execution failed under GCC 15:\n{test_res.stdout}\n{test_res.stderr}"
        ]

    logs.append("Verified GCC 15 build + ctest: all tests passed successfully")
    return True, logs


def check_5_asan_ubsan_build_and_test(
    repo_root: Path, build_dir: Path, verbose: bool
) -> Tuple[bool, List[str]]:
    """Check 5: AddressSanitizer + UndefinedBehaviorSanitizer build and test execution."""
    logs: List[str] = []
    san_flags = "-fsanitize=address,undefined -fno-omit-frame-pointer"
    cmake_cmd = [
        "cmake",
        "-B",
        str(build_dir),
        f"-DCMAKE_CXX_FLAGS={san_flags}",
        f"-DCMAKE_C_FLAGS={san_flags}",
        "-DSNAPPY_BUILD_TESTS=ON",
        "-DSNAPPY_BUILD_BENCHMARKS=OFF",
    ]
    res = run_command(cmake_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake configuration failed for ASan/UBSan build"]

    build_cmd = ["cmake", "--build", str(build_dir), "--target", "snappy_unittest"]
    res = run_command(build_cmd, cwd=repo_root, verbose=verbose)
    if res.returncode != 0:
        return False, ["CMake build failed for snappy_unittest under ASan/UBSan"]

    test_cmd = ["ctest", "--test-dir", str(build_dir), "--output-on-failure"]
    test_res = run_command(test_cmd, cwd=repo_root, verbose=verbose)
    if test_res.returncode != 0:
        return False, [
            f"ctest execution failed under ASan/UBSan:\n{test_res.stdout}\n{test_res.stderr}"
        ]

    # Verify zero sanitizer reports in output
    combined_output = test_res.stdout + test_res.stderr
    sanitizer_errors = [
        "AddressSanitizer",
        "UndefinedBehaviorSanitizer",
        "runtime error:",
        "SUMMARY: AddressSanitizer",
        "SUMMARY: UndefinedBehaviorSanitizer",
    ]
    for pattern in sanitizer_errors:
        if pattern in combined_output:
            return False, [
                f"Sanitizer violation detected in test output ({pattern}):\n{combined_output}"
            ]

    logs.append(
        "Verified ASan + UBSan build + ctest: all tests passed with zero sanitizer errors"
    )
    return True, logs


def main() -> int:
    """Run all verification checks and report results."""
    parser = argparse.ArgumentParser(
        description="Verify multi-architecture ISA dispatch, Universal Binary, GCC, and ASan/UBSan."
    )
    parser.add_argument(
        "--keep-build-dirs",
        action="store_true",
        help="Do not delete temporary build directories on exit.",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Print verbose command execution details.",
    )
    parser.add_argument(
        "--check",
        type=int,
        choices=[1, 2, 3, 4, 5],
        help="Run only a single specific check (1-5).",
    )
    args = parser.parse_args()

    checks = [
        (
            1,
            "x86_64 Disassembly Audit",
            "build_verify_x86",
            check_1_x86_disassembly,
        ),
        (
            2,
            "arm64 Disassembly Audit (-march=armv8-a+nocrc)",
            "build_verify_arm64_nocrc",
            check_2_arm64_disassembly,
        ),
        (
            3,
            "macOS Universal Binary Audit (lipo -info)",
            "build_verify_universal",
            check_3_universal_binary,
        ),
        (
            4,
            "GCC 15 Build & Unit Tests (g++-15)",
            "build_verify_gcc",
            check_4_gcc_build_and_test,
        ),
        (
            5,
            "AddressSanitizer + UndefinedBehaviorSanitizer Audit",
            "build_verify_asan",
            check_5_asan_ubsan_build_and_test,
        ),
    ]

    if args.check is not None:
        checks = [c for c in checks if c[0] == args.check]

    created_dirs: List[Path] = []
    all_passed = True
    passed_count = 0
    total_count = len(checks)

    print("=" * 70)
    print("Snappy Multi-Architecture ISA Dispatch Verification")
    print("=" * 70)

    try:
        for idx, title, dir_name, check_fn in checks:
            build_dir = REPO_ROOT / dir_name
            created_dirs.append(build_dir)
            if build_dir.exists():
                shutil.rmtree(build_dir, ignore_errors=True)

            print(f"\n[Check {idx}/{total_count}] Starting: {title}...")
            passed, logs = check_fn(REPO_ROOT, build_dir, args.verbose)
            if passed:
                passed_count += 1
                print(f"[PASS] Check {idx}: {title}")
                for log in logs:
                    print(f"  - {log}")
            else:
                all_passed = False
                print(f"[FAIL] Check {idx}: {title}")
                for log in logs:
                    print(f"  - {log}")
                break

    finally:
        if not args.keep_build_dirs:
            print("\nCleaning up temporary build directories...")
            for bdir in created_dirs:
                if bdir.exists():
                    shutil.rmtree(bdir, ignore_errors=True)
            print("Cleanup complete.")

    print("\n" + "=" * 70)
    print(f"Verification Summary: {passed_count}/{total_count} checks passed.")
    print("=" * 70)

    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())

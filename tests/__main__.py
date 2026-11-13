import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SECTIONS = ("crawler", "corpus", "text", "tokenizer", "lemmatizer", "zipf", "boolean")


def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("Must be positive")
    return number


def execute(command):
    print("\n> " + " ".join(map(str, command)), flush=True)
    try:
        return subprocess.run(list(map(str, command)), cwd=ROOT, check=False).returncode
    except OSError as error:
        print(f"Cannot execute command: {error}", file=sys.stderr)
        return 2


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run all Python and C++ project tests")
    parser.add_argument("--section", choices=SECTIONS, help="Run only one implementation section")
    parser.add_argument("--unit-only", action="store_true", help="Exclude C++ MongoDB, CLI and real dictionary checks")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-tests")
    parser.add_argument("--jobs", type=positive, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--cmake-arg", action="append", default=[], help="Extra CMake argument (use --cmake-arg=-DNAME=VALUE)")
    args = parser.parse_args(argv)
    failed = False
    if args.section is None or args.section in ("crawler", "corpus"):
        start = "tests" if args.section is None else "tests/" + args.section
        failed = execute([sys.executable, "-m", "unittest", "discover", "-s", start, "-t", ".", "-v"]) != 0
    if args.section in ("crawler", "corpus"):
        return int(failed)
    if not shutil.which("cmake") or not shutil.which("ctest"):
        print("C++ tests require cmake, ctest, a C++17 toolchain, ICU, OpenSSL, Cairo and the project dependencies.", file=sys.stderr)
        return 2
    build = args.build_dir.resolve()

    configure = ["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                 "-DBUILD_TESTING=ON", "-DTOKENIZER_WITH_MONGO=ON", "-DLEMMATIZER_BUILD=ON",
                 "-DLEMMATIZER_WITH_MONGO=ON", "-DZIPF_BUILD=ON", "-DBOOLEAN_BUILD=ON",
                 "-DBOOLEAN_BUILD_INDEX=ON", "-DBOOLEAN_BUILD_SEARCH=ON", "-DPYTHON_TESTS=OFF",
                 *args.cmake_arg]
    if execute(configure):
        return 1
    if execute(["cmake", "--build", build, "--parallel", args.jobs]):
        return 1
    command = ["ctest", "--test-dir", build, "--output-on-failure", "--no-tests=error", "--parallel", args.jobs]
    if args.section:
        command.extend(["-L", "^" + args.section + "$"])
    if args.unit_only:
        command.extend(["-LE", "^(integration|dictionary|cli)$"])
    failed = execute(command) != 0 or failed
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())

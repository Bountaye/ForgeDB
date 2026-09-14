"""Configure, build and test; normalize Windows PATH casing for MSBuild."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", default="build")
parser.add_argument("--type", default="Release")
parser.add_argument("--sanitizer", default="")
args = parser.parse_args()
env = {k.upper(): v for k, v in os.environ.items()} if os.name == "nt" else dict(os.environ)
cmake = shutil.which("cmake")
if not cmake and os.name == "nt":
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    root = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip()
    cmake = str(Path(root) / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe")
if not cmake:
    raise SystemExit("Install CMake and a C++20 compiler first")
configure = [cmake, "-S", ".", "-B", args.build, f"-DFORGEDB_SANITIZER={args.sanitizer}"]
if os.name == "nt":
    configure += ["-A", "x64"]
else:
    configure += [f"-DCMAKE_BUILD_TYPE={args.type}"]
subprocess.run(configure, env=env, check=True)
subprocess.run([cmake, "--build", args.build, "--config", args.type, "--parallel", "4"], env=env, check=True)
subprocess.run([str(Path(cmake).with_name("ctest.exe" if os.name == "nt" else "ctest")), "--test-dir", args.build, "-C", args.type, "--output-on-failure"], env=env, check=True)

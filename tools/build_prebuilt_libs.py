#!/usr/bin/env python3
import argparse
import hashlib
import pathlib
import re
import shlex
import subprocess
import sys
from typing import Dict, Iterable, List, Tuple


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEP_INDEX = ROOT / "Objects" / "A_i2s_capture_Target 1.dep"
PREBUILT_DIR = ROOT / "prebuilt"
PREBUILT_LIB_DIR = PREBUILT_DIR / "lib"
PREBUILT_OBJ_DIR = PREBUILT_DIR / "obj"
OBJECTS_DIR = ROOT / "Objects"
UVPROJX = ROOT / "A_i2s_capture.uvprojx"
ARMCLANG = (ROOT / ".." / ".." / "ARM" / "ARMCLANG" / "bin" / "armclang.exe").resolve()
ARMAR = (ROOT / ".." / ".." / "ARM" / "ARMCLANG" / "bin" / "armar.exe").resolve()

LIBRARIES = ("tflm", "cmsis_nn", "cmsis_dsp")
LIBRARY_OUTPUTS = {
    "tflm": PREBUILT_LIB_DIR / "tflm.lib",
    "cmsis_nn": PREBUILT_LIB_DIR / "cmsis_nn.lib",
    "cmsis_dsp": PREBUILT_LIB_DIR / "cmsis_dsp.lib",
}


def normalize_path(text: str) -> str:
    return text.replace("/", "\\").lower()


def classify_source(source: str) -> str:
    normalized = normalize_path(source)
    if "cmsis-nn" in normalized:
        return "cmsis_nn"
    if "cmsis-dsp" in normalized:
        return "cmsis_dsp"
    if ("tensorflow-lite-micro" in normalized) or ("rte\\machine_learning" in normalized):
        return "tflm"
    return ""


def discover_sources() -> Dict[str, List[str]]:
    buckets: Dict[str, List[str]] = {name: [] for name in LIBRARIES}
    seen = {name: set() for name in LIBRARIES}

    project_text = UVPROJX.read_text(encoding="utf-8", errors="ignore")
    group_match = re.search(r"<Group>\s*<GroupName>TFLM Core Engine</GroupName>\s*<Files>(.*?)</Files>\s*</Group>", project_text, re.DOTALL)
    if group_match:
        for path_match in re.finditer(r"<FilePath>(.*?)</FilePath>", group_match.group(1)):
            source = path_match.group(1).strip()
            if source and source not in seen["tflm"]:
                seen["tflm"].add(source)
                buckets["tflm"].append(source)

    common_match = re.search(r"<FilePath>(\.\.\\\.\.\\tensorflow\\tensorflow-lite-micro\\1\.25\.2\\tensorflow\\lite\\core\\c\\common\.cpp)</FilePath>", project_text)
    if common_match:
        source = common_match.group(1).strip()
        if source not in seen["tflm"]:
            seen["tflm"].add(source)
            buckets["tflm"].append(source)

    for source in (
        r".\RTE\Machine_Learning\debug_log.cpp",
        r".\RTE\Machine_Learning\micro_time.cpp",
        r".\RTE\Machine_Learning\system_setup.cpp",
    ):
        if source not in seen["tflm"]:
            seen["tflm"].add(source)
            buckets["tflm"].append(source)

    for dep_file in sorted(OBJECTS_DIR.glob("*.d")):
        text = dep_file.read_text(encoding="utf-8", errors="ignore").replace("\\\n", " ")
        if ":" not in text:
            continue
        source = text.split(":", 1)[1].strip().split()[0].strip()
        lib_name = classify_source(source)
        if lib_name not in ("cmsis_nn", "cmsis_dsp"):
            continue
        if source in seen[lib_name]:
            continue
        seen[lib_name].add(source)
        buckets[lib_name].append(source)

    return buckets


def parse_dep_index_entries() -> Iterable[Tuple[str, str]]:
    text = DEP_INDEX.read_text(encoding="utf-8", errors="ignore")
    pattern = re.compile(r"^F \((.*?)\)\((.*?)\)\((.*?)(?=^[FI] \(|\Z)", re.MULTILINE | re.DOTALL)

    for match in pattern.finditer(text):
        source = match.group(1).strip()
        args = match.group(3).strip()
        if args.endswith(")"):
            args = args[:-1].rstrip()
        yield source, args


def strip_output_tokens(tokens: List[str]) -> List[str]:
    stripped: List[str] = []
    skip_next = False
    for token in tokens:
        if skip_next:
            skip_next = False
            continue
        if token == "-o":
            skip_next = True
            continue
        if token == "-MMD":
            continue
        stripped.append(token)
    return stripped


def load_compile_templates() -> Dict[str, List[str]]:
    templates: Dict[str, List[str]] = {}

    for source, args in parse_dep_index_entries():
        if not classify_source(source):
            continue

        lang = "cpp" if source.lower().endswith((".cpp", ".cc", ".cxx")) else "c"
        if lang in templates:
            continue

        tokens = shlex.split(" ".join(args.split()), posix=False)
        tokens = strip_output_tokens(tokens)
        templates[lang] = tokens

    if "c" not in templates or "cpp" not in templates:
        raise RuntimeError("Failed to extract C/C++ compile templates from dependency index.")

    return templates


def ensure_preincludes(tokens: List[str]) -> List[str]:
    include_flag = "-ID:/keil_v5/keil/ARM/CMSIS-NN/7.0.0/Include"
    required = [
        ("-include", "./RTE/_Target_1/Pre_Include_Global.h"),
        ("-include", "./RTE/_Target_1/RTE_Components.h"),
    ]
    flattened = " ".join(tokens)
    result = list(tokens)
    if include_flag not in result:
        result.append(include_flag)
    for flag, value in required:
        if value not in flattened:
            result.extend([flag, value])
    return result


def object_name_for_source(source: str) -> str:
    stem = pathlib.Path(source).stem
    digest = hashlib.sha1(normalize_path(source).encode("utf-8")).hexdigest()[:10]
    return f"{stem}_{digest}.o"


def object_is_fresh(output: pathlib.Path, source: pathlib.Path, dependencies: Iterable[pathlib.Path]) -> bool:
    if not output.exists():
        return False

    output_mtime = output.stat().st_mtime
    if source.stat().st_mtime > output_mtime:
        return False

    for dep in dependencies:
        if dep.exists() and dep.stat().st_mtime > output_mtime:
            return False

    return True


def run(cmd: List[str], cwd: pathlib.Path) -> None:
    subprocess.run(cmd, cwd=str(cwd), check=True)


def build_objects(
    lib_name: str,
    sources: List[str],
    templates: Dict[str, List[str]],
    verbose: bool,
) -> List[pathlib.Path]:
    built_objects: List[pathlib.Path] = []
    obj_dir = PREBUILT_OBJ_DIR / lib_name
    obj_dir.mkdir(parents=True, exist_ok=True)

    dependencies = [DEP_INDEX, ROOT / "RTE" / "_Target_1" / "Pre_Include_Global.h", ROOT / "RTE" / "_Target_1" / "RTE_Components.h"]

    for source_text in sources:
        source = pathlib.Path(source_text)
        output = obj_dir / object_name_for_source(source_text)
        lang = "cpp" if source.suffix.lower() in (".cpp", ".cc", ".cxx") else "c"
        tokens = ensure_preincludes(templates[lang])

        if object_is_fresh(output, source, dependencies):
            built_objects.append(output)
            continue

        cmd = [str(ARMCLANG)] + tokens + ["-o", str(output), str(source)]
        if verbose:
            print("compile:", output.name)
        run(cmd, ROOT)
        built_objects.append(output)

    return built_objects


def archive_library(lib_name: str, members: List[pathlib.Path], verbose: bool) -> pathlib.Path:
    PREBUILT_LIB_DIR.mkdir(parents=True, exist_ok=True)
    output = LIBRARY_OUTPUTS[lib_name]
    response = PREBUILT_OBJ_DIR / f"{lib_name}_members.via"
    response.write_text("\n".join(str(member) for member in members) + "\n", encoding="utf-8")

    if output.exists():
        output.unlink()

    cmd = [str(ARMAR), "--create", "-c", str(output), "--via", str(response)]
    if verbose:
        print("archive:", output.name)
    run(cmd, ROOT)
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description="Build precompiled third-party libraries for the Keil project.")
    parser.add_argument("--verbose", action="store_true", help="Show each compile/archive action.")
    parser.add_argument("--list", action="store_true", help="Only print discovered source counts.")
    args = parser.parse_args()

    if not DEP_INDEX.exists():
        print(f"Missing dependency index: {DEP_INDEX}", file=sys.stderr)
        return 2

    if not ARMCLANG.exists() or not ARMAR.exists():
        print("armclang/armar not found under ../../ARM/ARMCLANG/bin", file=sys.stderr)
        return 2

    sources = discover_sources()
    for lib_name in LIBRARIES:
        if not sources[lib_name]:
            print(f"No sources discovered for {lib_name}. Build the original target once to refresh {DEP_INDEX.name}.", file=sys.stderr)
            return 2

    if args.list:
        for lib_name in LIBRARIES:
            print(f"{lib_name}: {len(sources[lib_name])}")
        return 0

    templates = load_compile_templates()
    built_libs: List[pathlib.Path] = []

    for lib_name in LIBRARIES:
        members = build_objects(lib_name, sources[lib_name], templates, args.verbose)
        built_libs.append(archive_library(lib_name, members, args.verbose))

    for lib in built_libs:
        print(f"built {lib.relative_to(ROOT)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())

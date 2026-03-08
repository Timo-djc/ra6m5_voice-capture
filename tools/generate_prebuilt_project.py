#!/usr/bin/env python3
import pathlib
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_PROJECT = ROOT / "A_i2s_capture.uvprojx"
OUTPUT_PROJECT = ROOT / "A_i2s_capture_prebuilt.uvprojx"

EXTRA_INCLUDE_PATHS = [
    "d:/keil_v5/keil/ARM/CMSIS-DSP/1.17.0/Include",
    "d:/keil_v5/keil/ARM/CMSIS-DSP/1.17.0/PrivateInclude",
    "d:/keil_v5/keil/ARM/CMSIS-NN/7.0.0/Include",
    "d:/keil_v5/keil/tensorflow/tensorflow-lite-micro/1.25.2",
    "d:/keil_v5/keil/tensorflow/tensorflow-lite-micro/1.25.2/tensorflow/lite/micro/testing",
]

LIB_ARGS = (
    ' ".\\prebuilt\\lib\\tflm.lib"'
    ' ".\\prebuilt\\lib\\cmsis_nn.lib"'
    ' ".\\prebuilt\\lib\\cmsis_dsp.lib"'
)

PREINCLUDE_ARGS = " -include ./RTE/_Target_1/Pre_Include_Global.h -include ./RTE/_Target_1/RTE_Components.h"


def is_third_party_file(path_text: str) -> bool:
    normalized = path_text.replace("/", "\\").lower()
    return ("tensorflow-lite-micro" in normalized) or ("rte\\machine_learning" in normalized)


def dedupe_semicolon_paths(raw: str, extras) -> str:
    parts = [part for part in (raw or "").split(";") if part]
    normalized = {part.lower(): part for part in parts}
    for extra in extras:
        if extra.lower() not in normalized:
            parts.append(extra)
    return ";".join(parts)


def indent(elem, level=0):
    indent_text = "\n" + level * "  "
    if len(elem):
        if not elem.text or not elem.text.strip():
            elem.text = indent_text + "  "
        for child in elem:
            indent(child, level + 1)
        if not child.tail or not child.tail.strip():
            child.tail = indent_text
    if level and (not elem.tail or not elem.tail.strip()):
        elem.tail = indent_text


def main() -> None:
    tree = ET.parse(SOURCE_PROJECT)
    root = tree.getroot()

    target = root.find("./Targets/Target")
    target.find("TargetName").text = "Target 1 (Prebuilt Libs)"

    common = target.find("./TargetOption/TargetCommonOption")
    common.find("OutputDirectory").text = r".\Objects_prebuilt\\"
    common.find("OutputName").text = "A_i2s_capture_prebuilt"
    common.find("ListingPath").text = r".\Listings_prebuilt\\"
    common.find("IncludePath").text = dedupe_semicolon_paths(common.findtext("IncludePath", ""), EXTRA_INCLUDE_PATHS)

    before_make = common.find("BeforeMake")
    before_make.find("RunUserProg1").text = "1"
    before_make.find("UserProg1Name").text = r'powershell.exe -ExecutionPolicy Bypass -File ".\tools\build_prebuilt_libs.ps1"'

    c_include = target.find("./TargetOption/TargetArmAds/Cads/VariousControls/IncludePath")
    c_include.text = dedupe_semicolon_paths(c_include.text or "", EXTRA_INCLUDE_PATHS)

    a_include = target.find("./TargetOption/TargetArmAds/Aads/VariousControls/IncludePath")
    a_include.text = dedupe_semicolon_paths(a_include.text or "", EXTRA_INCLUDE_PATHS)

    c_misc = target.find("./TargetOption/TargetArmAds/Cads/VariousControls/MiscControls")
    misc_text = c_misc.text or ""
    if PREINCLUDE_ARGS.strip() not in misc_text:
        misc_text += PREINCLUDE_ARGS
    c_misc.text = misc_text.strip()

    linker_misc = target.find("./TargetOption/TargetArmAds/LDads/Misc")
    ld_text = linker_misc.text or ""
    if LIB_ARGS.strip() not in ld_text:
        ld_text += LIB_ARGS
    linker_misc.text = ld_text.strip()

    groups = target.find("./Groups")
    for group in list(groups):
        group_name = group.findtext("GroupName", "")
        if group_name == "TFLM Core Engine":
            groups.remove(group)
            continue

        files = group.find("Files")
        if files is None:
            continue

        for file_node in list(files):
            path_text = file_node.findtext("FilePath", "")
            if is_third_party_file(path_text):
                files.remove(file_node)

        if len(files) == 0:
            groups.remove(group)

    rte_components = root.find("./RTE/components")
    for component in list(rte_components):
        cclass = component.get("Cclass", "")
        cgroup = component.get("Cgroup", "")
        if (cclass == "CMSIS" and cgroup == "DSP") or (cclass == "Machine Learning"):
            rte_components.remove(component)

    rte_files = root.find("./RTE/files")
    for file_node in list(rte_files):
        rte_files.remove(file_node)

    indent(root)
    tree.write(OUTPUT_PROJECT, encoding="UTF-8", xml_declaration=True)


if __name__ == "__main__":
    main()

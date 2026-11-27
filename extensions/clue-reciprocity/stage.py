#!/usr/bin/env python3
"""Stage the clue reciprocity extension in disposable MaaMeow and MaaCore trees."""

import argparse
import os
import shutil
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
PACKAGE = Path("app/src/main/java/com/aliothmoon/maameow")
CORE_TASKS = Path("src/MaaCore/Task")
ABI_MACHINE = {"arm64-v8a": 183, "x86_64": 62}


def replace_once(text, old, new, path):
    if text.count(old) != 1:
        raise ValueError(f"Expected one integration point in {path}: {old.splitlines()[0]}")
    return text.replace(old, new, 1)


def plan_app(root):
    config_path = root / PACKAGE / "presentation/view/panel/ConfigurationPanel.kt"
    panel_path = root / PACKAGE / "presentation/view/panel/InfrastConfigPanel.kt"
    analyze_path = root / PACKAGE / "domain/usecase/AnalyzeTaskChainUseCase.kt"
    already_staged = (
        "reciprocityClientType = clientType" in config_path.read_text(),
        "extension.ClueReciprocitySetting(" in panel_path.read_text(),
        "extension.ClueReciprocity.taskParams(" in analyze_path.read_text(),
    )
    if all(already_staged):
        return {}
    if any(already_staged):
        raise ValueError("Incomplete app hooks. Use a fresh MaaMeow staging tree.")
    changes = {}
    changes[config_path] = replace_once(
        config_path.read_text(),
        "is InfrastConfig -> InfrastConfigPanel(\n                            config = cfg, onConfigChange = onConfigChange",
        """is InfrastConfig -> InfrastConfigPanel(
                            config = cfg, onConfigChange = onConfigChange,
                            reciprocityTaskId = selectedNode.id, reciprocityClientType = clientType""",
        config_path,
    )
    panel = replace_once(
        panel_path.read_text(),
        "config: InfrastConfig, onConfigChange: (InfrastConfig) -> Unit, modifier: Modifier = Modifier",
        'config: InfrastConfig, onConfigChange: (InfrastConfig) -> Unit, modifier: Modifier = Modifier,\n'
        '    reciprocityTaskId: String? = null, reciprocityClientType: String = ""',
        panel_path,
    )
    anchor = "                            item {\n                                ReceptionSendClueSection(config, onConfigChange)\n                            }"
    changes[panel_path] = replace_once(
        panel, anchor, anchor + """
                            if (reciprocityTaskId != null) {
                                item {
                                    com.aliothmoon.maameow.extension.ClueReciprocitySetting(
                                        reciprocityTaskId, reciprocityClientType
                                    )
                                }
                            }""", panel_path
    )
    changes[analyze_path] = replace_once(
        analyze_path.read_text(),
        "task.copy(slot = TaskSlot(node.id, index))",
        """com.aliothmoon.maameow.extension.ClueReciprocity.taskParams(
                    node.config, node.id, taskChainState.profileId.value, clientType, task
                ).copy(slot = TaskSlot(node.id, index))""",
        analyze_path,
    )
    return changes


def plan_core(root):
    reception = root / CORE_TASKS / "Infrast/InfrastReceptionTask.cpp"
    header = reception.with_suffix(".h")
    interface = root / CORE_TASKS / "Interface/InfrastTask.cpp"
    already_staged = (
        '#include "Pylinx/ClueReciprocityTask.h"' in reception.read_text(),
        "set_clue_reciprocity_key(" in header.read_text(),
        "set_clue_reciprocity_key(" in interface.read_text(),
    )
    if all(already_staged):
        return {}
    if any(already_staged):
        raise ValueError("Incomplete native hooks. Use a fresh MaaCore staging tree.")
    source = replace_once(
        reception.read_text(), '#include "InfrastReceptionTask.h"',
        '#include "InfrastReceptionTask.h"\n#include "Pylinx/ClueReciprocityTask.h"', reception
    )
    start = source.index("    get_friend_clue();\n")
    end = source.index("    // 线索交流关闭时", start)
    stock_path = source[start:end].rstrip()
    native_hook = """    if (!m_clue_reciprocity_key.empty()) {
        const bool clue_steps_complete = clue_reciprocity::ClueReciprocityTask(*this).run(
            m_clue_reciprocity_key, [this] { return get_self_clue(); });
        if (need_exit()) {
            return false;
        }
        if (!clue_steps_complete) {
            Log.warn("PylinxClueReciprocity", "clue step stopped; not reentering reception");
            return true;
        }
        set_product("General");
        if (!m_skip_shift && !shift() && !need_exit()) {
            discard_pending_selection();
            Log.warn("PylinxClueReciprocity", "staff selection incomplete; not rerunning clues");
        }
        return !need_exit();
    }
    else {
""" + "\n".join("    " + line if line else "" for line in stock_path.splitlines()) + "\n    }\n\n"
    return {
        reception: source[:start] + native_hook + source[end:],
        header: replace_once(
            header.read_text(), "protected:\n",
            "    void set_clue_reciprocity_key(std::string key) { m_clue_reciprocity_key = key; }\n\n"
            "protected:\n", header,
        ).replace("    bool m_receive_message_board", "    std::string m_clue_reciprocity_key;\n    bool m_receive_message_board", 1),
        interface: replace_once(
            interface.read_text(), "    m_reception_task_ptr->set_send_clue(reception_send_clue);",
            "    m_reception_task_ptr->set_send_clue(reception_send_clue);\n"
            '    m_reception_task_ptr->set_clue_reciprocity_key(params.get("reception_clue_reciprocity", std::string {}));',
            interface,
        ),
    }


def install_core(app, abi, library):
    data = library.read_bytes()
    if (
        data[:6] != b"\x7fELF\x02\x01"
        or int.from_bytes(data[18:20], "little") != ABI_MACHINE[abi]
        or b"PylinxClueReciprocity" not in data
    ):
        raise ValueError(f"{library} is not a reciprocity-enabled {abi} MaaCore library")
    destination = app / "app/src/main/jniLibs" / abi / "libMaaCore.so"
    if not destination.is_file():
        raise ValueError("Run the app's standard MaaCore resource deployment in the staged tree first.")
    shutil.copyfile(library, destination)
    marker = app / f"app/src/main/assets/pylinx/clue-reciprocity-{abi}"
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text("1\n")


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--app-tree", type=Path, required=True)
    cli.add_argument("--core-tree", type=Path)
    cli.add_argument("--install-core", nargs=2, metavar=("ABI", "LIBMAACORE"))
    cli.add_argument("--apply", action="store_true")
    cli.add_argument("--ci-checkout", action="store_true", help="Allow staging directly in a GitHub Actions checkout")
    args = cli.parse_args()
    app = args.app_tree.resolve()
    is_ci_checkout = (
        args.ci_checkout
        and os.environ.get("GITHUB_ACTIONS") == "true"
        and app == Path(os.environ["GITHUB_WORKSPACE"]).resolve()
    )
    if (app == REPO and not is_ci_checkout) or (args.core_tree and args.core_tree.resolve() == app):
        cli.error("Use separate staging trees. The source branch will not be rewritten.")
    if args.install_core:
        abi, library = args.install_core
        if args.core_tree or abi not in ABI_MACHINE or not args.apply:
            cli.error("--install-core requires --apply, a supported ABI, and no --core-tree")
        install_core(app, abi, Path(library))
        return

    plan = plan_app(app)
    if args.core_tree:
        plan.update(plan_core(args.core_tree))
    for path, content in plan.items():
        print(path)
        if args.apply:
            path.write_text(content)
    if args.apply:
        for relative in [
            PACKAGE / "extension/ClueReciprocity.kt",
            Path("app/src/main/res/values/pylinx_clue_reciprocity.xml"),
            Path("app/src/main/res/values-en/pylinx_clue_reciprocity.xml"),
        ]:
            destination = app / relative
            if app != REPO:
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(REPO / relative, destination)
        if args.core_tree:
            shutil.copytree(HERE / "core", args.core_tree / CORE_TASKS / "Infrast/Pylinx", dirs_exist_ok=True)


if __name__ == "__main__":
    main()

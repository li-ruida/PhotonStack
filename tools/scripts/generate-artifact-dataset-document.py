#!/usr/bin/env python3
"""Generate the Chinese artifact benchmark maintenance document."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history", type=Path, default=Path("tests/artifact-blindset/history.json"))
    parser.add_argument("--base-manifest", type=Path, default=Path("tests/artifact-blindset/manifest.json"))
    parser.add_argument("--rounds-dir", type=Path, default=Path("tests/artifact-blindset/rounds"))
    parser.add_argument("--report-dir", type=Path, default=Path("build/artifact-blindset"))
    parser.add_argument(
        "--desktop-report",
        type=Path,
        default=Path("build/artifact-blindset/desktop-final.json"),
    )
    parser.add_argument(
        "--large-corpus",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22.json"),
    )
    parser.add_argument(
        "--large-first-run",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22-first-run-summary.json"),
    )
    parser.add_argument(
        "--ai-review",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22-ai-review.json"),
    )
    parser.add_argument(
        "--ai-first-run",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22-ai-first-run-summary.json"),
    )
    parser.add_argument(
        "--ai-current",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22-ai-current-summary.json"),
    )
    parser.add_argument(
        "--ai-review-development",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22-ai-review-development.json"),
    )
    parser.add_argument(
        "--ai-first-run-development",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22-ai-first-run-summary-development.json"),
    )
    parser.add_argument(
        "--ai-current-development",
        type=Path,
        default=Path("tests/artifact-blindset/corpus-v22-ai-current-summary-development.json"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("docs/testing/artifact-removal-dataset.zh-CN.md"),
    )
    return parser.parse_args()


def load_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"无法读取 {path}: {error}") from error


def expectation_text(expectation: dict) -> str:
    minimum = expectation.get("minimumArtifacts")
    maximum = expectation.get("maximumArtifacts")
    if minimum is not None and maximum is not None:
        return f"{minimum}–{maximum} 条"
    if minimum is not None:
        return f"至少 {minimum} 条"
    return f"最多 {maximum} 条"


def report_by_id(path: Path) -> dict[str, dict]:
    report = load_json(path)
    return {case["id"]: case for case in report.get("cases", [])}


def main() -> int:
    arguments = parse_arguments()
    history = load_json(arguments.history)
    if history.get("schemaVersion") != 1:
        raise SystemExit("history schemaVersion 必须为 1")

    manifests: list[tuple[str, Path, dict]] = [
        ("基础集", arguments.base_manifest, load_json(arguments.base_manifest))
    ]
    round_history = history.get("rounds", [])
    history_ids = [item["id"] for item in round_history]
    for round_id in history_ids:
        path = arguments.rounds_dir / f"{round_id}.json"
        manifests.append((round_id, path, load_json(path)))

    for item, (label, _, document) in zip(round_history, manifests[1:]):
        cases = document.get("cases", [])
        identifiers = {case.get("id") for case in cases}
        failures = set(item.get("firstRunFailures", []))
        if item.get("total") != len(cases):
            raise SystemExit(f"{label}: history total 与清单数量不一致")
        if item.get("firstRunPassed") != len(cases) - len(failures):
            raise SystemExit(f"{label}: history 首次通过数与失败列表不一致")
        if not failures.issubset(identifiers):
            raise SystemExit(f"{label}: history 包含清单中不存在的失败样本")

    reports: dict[str, dict[str, dict]] = {
        "基础集": report_by_id(arguments.report_dir / "base-final.json")
    }
    for round_id in history_ids:
        reports[round_id] = report_by_id(arguments.report_dir / f"{round_id}-final.json")

    network_total = sum(len(document.get("cases", [])) for _, _, document in manifests)
    current_passed = sum(
        1
        for label, _, document in manifests
        for case in document.get("cases", [])
        if reports[label].get(case["id"], {}).get("passed") is True
    )
    first_passed = sum(item["firstRunPassed"] for item in round_history)
    first_total = sum(item["total"] for item in round_history)
    desktop_report = load_json(arguments.desktop_report)
    desktop = desktop_report.get("desktop", [])
    desktop_errors = desktop_report.get("desktopFailed")
    large_corpus = load_json(arguments.large_corpus)
    large_cases = large_corpus.get("cases", [])
    large_first_run = load_json(arguments.large_first_run)
    ai_review = load_json(arguments.ai_review)
    ai_first_run = load_json(arguments.ai_first_run)
    ai_current = load_json(arguments.ai_current)
    ai_review_development = load_json(arguments.ai_review_development)
    ai_first_run_development = load_json(arguments.ai_first_run_development)
    ai_current_development = load_json(arguments.ai_current_development)
    reviewed_total = ai_review["summary"]["reviewed"] + ai_review_development["summary"]["reviewed"]
    high_confidence_total = (
        ai_review["summary"]["highConfidenceBenchmarkCases"] +
        ai_review_development["summary"]["highConfidenceBenchmarkCases"]
    )
    manual_queue_total = (
        ai_review["summary"]["manualQueueCases"] +
        ai_review_development["summary"]["manualQueueCases"]
    )
    ai_baseline_processed = ai_first_run["processed"] + ai_first_run_development["processed"]
    ai_baseline_passed = ai_first_run["passed"] + ai_first_run_development["passed"]
    ai_current_processed = ai_current["processed"] + ai_current_development["processed"]
    ai_current_passed = ai_current["passed"] + ai_current_development["passed"]
    def combined_dispositions(names: tuple[str, ...]) -> tuple[int, int]:
        cases = 0
        passed = 0
        for report in (ai_current, ai_current_development):
            for name in names:
                disposition = report["byDisposition"].get(name, {"cases": 0, "passed": 0})
                cases += disposition["cases"]
                passed += disposition["passed"]
        return passed, cases

    removable_mixed_passed, removable_mixed_cases = combined_dispositions(("removable", "mixed"))
    protected_none_passed, protected_none_cases = combined_dispositions(("protected", "none"))
    large_sources = Counter(case["sourceCategory"] for case in large_cases)

    lines = [
        "# PhotonStack 卫星/伪影去除测试数据集",
        "",
        f"> 自动生成于 {history['updated']}。仓库内版本与桌面副本由同一脚本维护。",
        "",
        "## 当前结论",
        "",
        f"- 严格逐图数量边界网络集共 {network_total} 张；当前代码通过 {current_passed}/{network_total}。",
        f"- v22 大规模弱标签集共 {len(large_cases)} 张；首轮固定运行 {large_first_run['attempted']} 张，成功处理 {large_first_run['processed']}/{large_first_run['attempted']}，运行错误 {large_first_run['processingFailed']}。",
        f"- v22 与 Commons 分类弱标签一致 {large_first_run['weakLabelConforming']}/{large_first_run['attempted']}；分类会混入书籍扫描、城市夜景、合成图或同时含多类轨迹，因此该数字只用于人工审核排队，不是准确率。",
        f"- AI 已逐图复核全部 {reviewed_total} 张：{high_confidence_total} 张达到高置信可评测标准，{manual_queue_total} 张进入人工复核/剔除队列。修改前基线为 {ai_baseline_passed}/{ai_baseline_processed}，当前为 {ai_current_passed}/{ai_current_processed}（{ai_current_passed / ai_current_processed:.1%}）。AI 标签仍是首轮筛选，不冒充人工真值。",
        f"- v16–v21 六轮真正首次盲测通过 {first_passed}/{first_total}（{first_passed / first_total:.1%}）。这个数字用于监控泛化，不能被修复后的回归成绩替代。",
        f"- 桌面逐图覆盖 {len(desktop)} 张，运行错误 {desktop_errors} 个；桌面图片目前只有检测数量，没有人工像素标注，因此不计入通过率。",
        "- 最终验收使用 `/Applications/PhotonStack.app` 内的 CLI 和真实 macOS 图像解码环境；安装包、打包产物与测试构建的 CLI 哈希一致。",
        "- `DSC_6695.NEF` 已用最终安装版执行实际去除：检测 2 条、移除 2 条；其中底边候选长约 531 像素，放大检查确认用户指出的横线已消失。",
        "- 当前数据只检查每张图的伪影数量上下界，尚不能证明每条轨迹像素都被完整覆盖，也不能证明所有额外候选都正确。",
        "",
        "## 首次盲测历史",
        "",
        "| 轮次 | 首次结果 | 首次失败样本 | 当前回归 |",
        "| --- | ---: | --- | ---: |",
    ]
    for item in round_history:
        round_id = item["id"]
        current = reports[round_id]
        current_round_passed = sum(result.get("passed") is True for result in current.values())
        failures = "、".join(f"`{identifier}`" for identifier in item["firstRunFailures"])
        lines.append(
            f"| {round_id} | {item['firstRunPassed']}/{item['total']} | {failures} | "
            f"{current_round_passed}/{item['total']} |"
        )
    lines.extend([
        "",
        "v21 的过程特意保留：首次漏掉 Gaia 的中性灰周期点链；加入新检测后，Kitt Peak 彩色地平线与 v18 过曝星轨又暴露误报。最终边界同时约束周期性、色差、暖色偏移和饱和度，并由合成正反例锁定。",
        "",
        "## v22：500 张大规模弱标签集",
        "",
        "该集合在首次运行前冻结：300 张为冻结评估部分，200 张原为保留开发部分；本轮按用户要求已将后 200 张全部纳入检测和 AI 复核。每项记录 Commons 来源页、许可证、作者/署名、原始尺寸、固定 1280px 下载地址、Commons SHA-1 和下载字节 SHA-256；图像字节只保存在忽略的构建缓存。",
        "",
        "| Commons 来源分类 | 清单数量 |",
        "| --- | ---: |",
    ])
    for source, count in sorted(large_sources.items()):
        lines.append(f"| `{source}` | {count} |")
    lines.extend([
        "",
        "| 首轮类别 | 尝试 | 成功处理 | 弱标签一致 | 弱标签不一致 |",
        "| --- | ---: | ---: | ---: | ---: |",
    ])
    for label, item in sorted(large_first_run["byCategoryLabel"].items()):
        lines.append(
            f"| `{label}` | {item['attempted']} | {item['processed']} | "
            f"{item['weakLabelConforming']} | {item['weakLabelNonconforming']} |"
        )
    lines.extend([
        "",
        "这里的“成功处理”表示最终安装版完成解码和检测并返回合法报告；“弱标签一致”只表示检测数量符合 Commons 分类推导出的粗略预期。只有经过人工逐图审核并写入专用边界或掩码的图片，才可升级为严格回归样本。",
        "",
        "首轮人工抽查同时看到了真实漏检与分类噪声：`Spacecraft (4934491199)`、`Mars and Milky Way` 等清晰轨迹确有漏检；另一方面，流星分类里混有书籍扫描和城市夜景，星轨图也可能同时包含卫星。为避免用错标数据反向破坏流星保护，本轮先保留原始报告和人工审核队列，不直接按 144/300 调阈值。",
        "",
        "## v22 AI 逐图复核",
        "",
        "AI 使用 1280px 图片生成的逐图联系表检查全部 500 张图片，并标记为应删除、应保护、混合、无轨迹、不适合或模糊。图表、插画、明显合成图、无关风景和低置信判断不会进入高置信评测清单。该复核能清理 Commons 类别噪声，但仍不是人工像素级真值。",
        "",
        "| AI 判断 | 高置信样本 | 修改前基线 | 当前通过 |",
        "| --- | ---: | ---: | ---: |",
    ])
    dispositions = sorted(set(ai_first_run["byDisposition"]) | set(ai_first_run_development["byDisposition"]))
    for disposition in dispositions:
        baseline_evaluation = ai_first_run["byDisposition"].get(disposition, {"cases": 0, "passed": 0})
        baseline_development = ai_first_run_development["byDisposition"].get(
            disposition, {"cases": 0, "passed": 0}
        )
        current_evaluation = ai_current["byDisposition"].get(disposition, {"passed": 0})
        current_development = ai_current_development["byDisposition"].get(disposition, {"passed": 0})
        cases = baseline_evaluation["cases"] + baseline_development["cases"]
        baseline_passed = baseline_evaluation["passed"] + baseline_development["passed"]
        current_passed = current_evaluation["passed"] + current_development["passed"]
        lines.append(
            f"| `{disposition}` | {cases} | {baseline_passed} | {current_passed} |"
        )
    lines.extend([
        "",
        f"高置信清单合计 {ai_current_processed} 张，修改前通过 {ai_baseline_passed} 张，当前通过 {ai_current_passed} 张、失败 {ai_current_processed - ai_current_passed} 张。当前应删除或混合场景通过 {removable_mixed_passed}/{removable_mixed_cases}；应保护或无轨迹场景通过 {protected_none_passed}/{protected_none_cases}。保护类改善明显，但剩余失败说明流星/星轨误报和复杂场景漏检都还没有解决。全部 500 张现已被查看，后续只能作为回归/开发集，不能再称为未见盲测。",
        "",
        "机器可读文件按 evaluation/development 两部分保存：`corpus-v22-ai-review*.json`（全部 500 张）、`corpus-v22-ai-high-confidence*.json`（349 张）、`corpus-v22-ai-manual-queue*.json`（151 张），以及修改前和当前的汇总报告。",
        "",
        "## 网络样本与当前结果",
        "",
        "图片字节不进入 Git。脚本只把固定尺寸版本下载到忽略的 `build/artifact-blindset/`，并用清单中的 SHA-256 校验。作者和许可证以来源页为准。",
        "",
        "| 集合 | 样本 | 期望 | 当前检测 | 结果 | 来源 |",
        "| --- | --- | ---: | ---: | --- | --- |",
    ])
    for label, _, document in manifests:
        for case in document["cases"]:
            result = reports[label].get(case["id"])
            if result is None:
                raise SystemExit(f"{label} 当前报告缺少 {case['id']}")
            status = "通过" if result.get("passed") else "失败"
            lines.append(
                f"| {label} | `{case['id']}` | {expectation_text(case['expectation'])} | "
                f"{result.get('trails')} | {status} | [Commons]({case['sourcePage']}) |"
            )

    lines.extend([
        "",
        "## 桌面逐图覆盖",
        "",
        "桌面原图由最终安装版 `/Applications/PhotonStack.app/Contents/MacOS/photonstack` 原地读取，不复制、不提交，也不作为训练数据。`伪影` 是检测候选数，`保护流星` 是被算法判为不能移除的候选数。",
        "",
        "| 文件 | 伪影 | 保护流星 |",
        "| --- | ---: | ---: |",
    ])
    for item in desktop:
        lines.append(f"| `{item['filename']}` | {item['trails']} | {item['protectedMeteors']} |")

    lines.extend([
        "",
        "## 如何复验和维护",
        "",
        "```sh",
        "cmake --build build/debug --target photonstack",
        "python3 tools/scripts/evaluate-artifact-blindset.py \\",
        "  --manifest tests/artifact-blindset/manifest.json --split all --download \\",
        "  --json-report build/artifact-blindset/base-final.json",
        "for manifest in tests/artifact-blindset/rounds/*.json; do",
        "  round=$(basename \"$manifest\" .json)",
        "  python3 tools/scripts/evaluate-artifact-blindset.py \\",
        "    --manifest \"$manifest\" --split evaluation --download \\",
        "    --json-report \"build/artifact-blindset/${round}-final.json\"",
        "done",
        "python3 tools/scripts/evaluate-artifact-blindset.py \\",
        "  --manifest tests/artifact-blindset/rounds/v21.json \\",
        "  --binary /Applications/PhotonStack.app/Contents/MacOS/photonstack \\",
        "  --split evaluation --desktop-dir \"$HOME/Desktop\" --require-desktop \\",
        "  --json-report build/artifact-blindset/desktop-final.json",
        "python3 tools/scripts/generate-artifact-dataset-document.py",
        "```",
        "",
        "大规模集合的复验命令：",
        "",
        "```sh",
        "python3 tools/scripts/evaluate-artifact-blindset.py \\",
        "  --manifest tests/artifact-blindset/corpus-v22.json \\",
        "  --binary /Applications/PhotonStack.app/Contents/MacOS/photonstack \\",
        "  --cache build/artifact-corpus/v22-special-1280 \\",
        "  --split evaluation --workers 4 --quiet \\",
        "  --json-report build/artifact-corpus/v22-current.json",
        "python3 tools/scripts/generate-artifact-ai-review-sheets.py",
        "python3 tools/scripts/generate-artifact-ai-review.py",
        "```",
        "",
        "新增下一轮时，先选择未用于调参的新来源，写入新的 `rounds/vNN.json`，冻结图片哈希和修改前的检测器源码哈希，再进行第一次检测。第一次结果必须先写入 `history.json`，之后才允许针对失败样本修改算法。",
        "",
        "更强的下一步是给桌面和网络图片建立人工审核的轨迹掩码或中心线标注，以测量召回率、误检率和清除残留，而不只比较数量。",
        "",
    ])

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

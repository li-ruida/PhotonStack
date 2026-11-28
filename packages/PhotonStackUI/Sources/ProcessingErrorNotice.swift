import Foundation
import SwiftUI

/// Keep the original diagnostic available while presenting a short, actionable summary.
struct ProcessingFailureSummary {
    struct Diagnostic: Decodable {
        var type: String?
        var code: String?
        var message: String
    }

    static func diagnostic(in text: String) -> Diagnostic? {
        // stderr may contain progress/log lines before a pretty-printed JSON error.
        // Scan balanced objects once, respecting braces inside JSON strings.
        let bytes = Array(text.utf8.prefix(65_536))
        var start: Int?, depth = 0
        var quoted = false, escaped = false
        var result: Diagnostic?
        for (index, byte) in bytes.enumerated() {
            if start == nil {
                if byte == 123 { start = index; depth = 1; quoted = false; escaped = false }
                continue
            }
            if quoted {
                if escaped { escaped = false }
                else if byte == 92 { escaped = true }
                else if byte == 34 { quoted = false }
                continue
            }
            if byte == 34 { quoted = true }
            else if byte == 123 { depth += 1 }
            else if byte == 125 {
                depth -= 1
                if depth == 0, let begin = start {
                    if let value = try? JSONDecoder().decode(Diagnostic.self, from: Data(bytes[begin...index])),
                       value.type == "error" || (value.type == nil && value.code != nil) {
                        result = value
                    }
                    start = nil
                }
            }
        }
        return result
    }

    static func message(_ raw: String, chinese: Bool) -> String {
        let diagnostic = diagnostic(in: raw)
        if chinese, let code = diagnostic?.code {
            switch code {
            case "MemoryAllocationFailed": return "内存不足，请减少并发数或照片数量后重试。"
            case "OutputConflictsWithInput": return "输出位置与原文件相同，请另选位置。"
            case "ImageDimensionsMismatch": return "图像尺寸不一致，请检查原片和校准帧。"
            case "AlreadyCalibrated": return "这些照片已经校准，请关闭重复校准后重试。"
            case "MosaicInsufficientOverlap": return "共同星点不足，请检查天区分组和重叠区域。"
            case "MosaicWcsInvalid": return "天区坐标无效，请重新解算或改用星点对齐。"
            case "MosaicWcsCanvasTooLarge": return "天区跨度过大，请检查分组、比例和坐标。"
            default: break
            }
        }
        let detail = (diagnostic?.message ?? raw).trimmingCharacters(in: .whitespacesAndNewlines)
        if chinese {
            let english = detail.lowercased()
            if english.contains("no such file") || english.contains("file doesn’t exist") || english.contains("file does not exist") {
                return "找不到文件，请检查位置或重新选择。"
            }
            if english.contains("permission denied") || english.contains("don’t have permission") || english.contains("do not have permission") {
                return "无法访问文件，请检查读取和保存权限。"
            }
            if english.contains("already exists") { return "输出文件已存在，请另选位置。" }
            if english.contains("source or product changed") || english.contains("input or product changed") || english.contains("report changed") {
                return "原文件或结果已被修改，请重新处理。"
            }
            if english.contains("timed out") || english.contains("timeout exceeded") { return "处理超时，请重试或减少照片数量。" }
            if english.contains("measurement regions overlap") { return "星点测量区域重叠，请选择距离更远的比较星。" }
            if english.contains("choose mono or rgb fits") { return "请选择单色或已去马赛克的 RGB FITS。" }
            // Preserve concise messages already written for the user.
            if detail.count <= 180, !detail.contains("\n"), !detail.contains("{ "),
               detail.unicodeScalars.contains(where: { (0x4E00...0x9FFF).contains($0.value) }) {
                return detail
            }
            return "操作未完成，请查看错误详情。"
        }
        if !detail.isEmpty, detail.count <= 240, !detail.contains("\n") { return detail }
        return "The operation failed. See error details."
    }
}

struct ProcessingErrorNotice: View {
    let message: String
    let chinese: Bool
    var body: some View {
        let summary = ProcessingFailureSummary.message(message, chinese: chinese)
        VStack(alignment: .leading, spacing: 5) {
            Label(summary, systemImage: "exclamationmark.triangle")
                .foregroundStyle(.red).textSelection(.enabled)
            if summary != message {
                DisclosureGroup(chinese ? "错误详情" : "Error details") {
                    ScrollView {
                        Text(message).font(.caption.monospaced()).textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }.frame(maxHeight: 110)
                }.foregroundStyle(.secondary)
            }
        }.font(.caption).frame(maxWidth: .infinity, alignment: .leading)
    }
}

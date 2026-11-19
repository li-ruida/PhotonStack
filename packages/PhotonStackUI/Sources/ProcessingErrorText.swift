import Foundation
import PhotonStackAppCore
import PhotonStackProcessing

extension PhotonStackWorkspaceModel {
    func processingErrorMessage(_ error: Error) -> String {
        let chinese = language == .simplifiedChinese
        func byteLimitLabel(_ bytes: UInt64) -> String {
            if bytes.isMultiple(of: 1_048_576) { return "\(bytes / 1_048_576) MiB" }
            return chinese ? "\(bytes) 字节" : "\(bytes) bytes"
        }
        switch error {
        case let StructuredDocumentError.fileTooLarge(bytes):
            let limit = byteLimitLabel(bytes)
            return chinese ? "文件超过大小上限（\(limit)）。" : "The file exceeds the size limit (\(limit))."
        case let ProjectRepositoryError.duplicateIdentifier(collection) where chinese:
            return "项目包含重复的标识符：\(collection)"
        case let error as ProcessingServiceError:
            switch error {
            case .executableNotFound:
                return chinese ? "找不到处理引擎，请重新安装 PhotonStack。" : "The processing engine is missing. Reinstall PhotonStack."
            case let .commandFailed(exitCode, stderr):
                if stderr.contains("MosaicInsufficientOverlap") {
                    let overlap = localizedMessage(error.localizedDescription)
                    if overlap != error.localizedDescription { return overlap }
                }
                struct Diagnostic: Decodable { var code: String?; var message: String }
                let diagnostic = try? JSONDecoder().decode(Diagnostic.self, from: Data(stderr.utf8))
                if chinese, let code = diagnostic?.code {
                    switch code {
                    case "MemoryAllocationFailed": return "可用内存不足。请减少并发数或处理较少的帧后重试。"
                    case "OutputConflictsWithInput": return "输出位置与输入文件相同，请另选位置。"
                    case "ImageDimensionsMismatch": return "图像尺寸不一致。请检查参与处理的原片和校准帧。"
                    case "AlreadyCalibrated": return "这些亮场已经完成校准。请关闭暗场、偏置、平场及固定纹理校正，直接进行配准和堆栈。"
                    case "MosaicWcsInvalid": return "无法使用天球坐标。请导入已解算的 TAN / TAN-SIP 母片，或切换为星点自动配准。\n" + (diagnostic?.message ?? "")
                    case "MosaicWcsCanvasTooLarge": return "这些母片的坐标跨度过大。请检查分组、像素比例和解算坐标。"
                    case "MosaicInsufficientOverlap": return "相邻天区没有足够的共同星点。请检查天区分组和重叠区域。"
                    default: break
                    }
                }
                let detail = diagnostic?.message ?? stderr
                return chinese ? "处理失败（退出码 \(exitCode)）：\(detail)" : "Processing failed (exit \(exitCode)): \(detail)"
            case let .commandOutputLimitExceeded(bytes):
                let limit = byteLimitLabel(UInt64(clamping: bytes))
                return chinese ? "报告超过 \(limit)，已停止处理。已有结果已保留。" : "Processing stopped because the report exceeded \(limit). Existing outputs were kept."
            case let .outputMissing(path):
                return chinese ? "未生成输出文件：\(path)" : "Output file was not created: \(path)"
            case let .outputInvalid(path):
                return chinese ? "无法读取输出图像：\(path)" : "Cannot read the output image: \(path)"
            case let .outputCommitFailed(path, reason):
                return chinese ? "无法保存到 \(path)：\(reason)" : "Could not save \(path): \(reason)"
            case let .invalidArguments(detail):
                return localizedMessage(detail)
            case let .invalidReport(command):
                return chinese ? "处理报告无效或不完整（\(command)）。" : "Invalid or incomplete report (\(command))."
            case let .invalidSelection(command):
                return chinese ? "当前检测结果中找不到所选区域，请重新选择（\(command)）。" : "The selection is missing from the current report. Select it again (\(command))."
            case let .inputColorEncodingMismatch(command, inputs):
                let names = inputs.map { URL(fileURLWithPath: $0).lastPathComponent }.joined(separator: ", ")
                return chinese ? "\(command) 的输入混用了 sRGB 和线性色彩，请先统一色彩空间：\(names)" : error.localizedDescription
            case .unsupportedPlatform:
                return chinese ? "此处理操作需要 macOS。" : "This operation requires macOS."
            }
        case let error as DisplayGridError where chinese:
            switch error {
            case .displayImageRequired: return "请使用已显影的 TIFF 或 PNG 进行细纹抑制，并以 TIFF 或 PNG 输出。"
            case .invalidSettings: return "细纹抑制强度应在 0 到 1 之间。"
            case .outputAlreadyExists: return "输出文件已存在，请另选位置。"
            }
        case let error as ContinuumToneError where chinese:
            switch error {
            case .invalidSettings: return "曲线须从 (0, 0) 单调上升至 (1, 1)，细节半径须小于外围半径。"
            case .displayImageRequired: return "请先将线性 FITS 显影，再调整弥散结构的明暗并导出显示图像。"
            }
        default:
            return localizedMessage(error.localizedDescription)
        }
    }
}

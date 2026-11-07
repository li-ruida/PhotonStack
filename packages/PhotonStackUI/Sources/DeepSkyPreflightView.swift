import Foundation
import PhotonStackProcessing
import SwiftUI

struct DeepSkyPreflightView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var expanded = false
    @State private var plan: DeepSkyResourcePlan?
    @State private var completedRequest: Request?
    @State private var refresh = 0
    @State private var checking = false
    @State private var error: String?
    private var chinese: Bool { model.language == .simplifiedChinese }
    struct Request: Equatable {
        let recipe: String?
        let destination: URL
        let refresh: Int

        init?(expanded: Bool, destination: URL, refresh: Int, recipe: @autoclosure () -> String?) {
            guard expanded else { return nil }
            self.destination = destination
            self.refresh = refresh
            self.recipe = recipe()
        }
    }
    var body: some View {
        // The full-workflow estimate opens every input header. Frame review
        // needs neither the recipe nor any of that I/O while this is collapsed.
        let destination = model.deepSkyWorkingDirectory
        let request = Request(expanded: expanded, destination: destination, refresh: refresh, recipe: try? model.deepSkyRecipeText())
        let currentPlan = request != nil && completedRequest == request ? plan : nil
        let needsAttention = currentPlan.map { $0.diskSpaceNeedsAttention || !$0.issues.isEmpty } ?? false
        DisclosureGroup(isExpanded: $expanded) {
            VStack(alignment: .leading, spacing: 6) {
                if let plan = currentPlan {
                    HStack(spacing: 20) {
                        metric(chinese ? "已统计输入体积" : "Counted source bytes", value: bytes(plan.sourceBytes))
                        metric(chinese ? "临时文件估算" : "Temporary estimate", value: bytes(plan.temporaryBytes))
                        metric(chinese ? "保留产物估算" : "Products estimate", value: bytes(plan.productBytes))
                        metric(chinese ? "磁盘可用" : "Disk available", value: bytes(plan.availableBytes))
                    }
                    Text(chinese
                        ? "堆栈主要缓冲区约 \(bytes(plan.stackBufferBytes))；筛片采样图约 \(bytes(plan.assessmentSampleBytes))。解码、配准、背景拟合及降噪还需要额外内存。"
                        : "Main stack buffers ≈ \(bytes(plan.stackBufferBytes)); assessment samples ≈ \(bytes(plan.assessmentSampleBytes)). Decoding, registration, background fitting and denoising require additional memory.")
                        .foregroundStyle(.secondary)
                    if plan.diskSpaceNeedsAttention {
                        Label(chinese ? "可用空间低于完整流程规划值，请在完整堆栈前整理空间或更换项目位置。" : "Available space is below the full-workflow estimate. Free space or move the project before a full stack.", systemImage: "externaldrive.badge.exclamationmark")
                            .foregroundStyle(.orange)
                    }
                    if !plan.issues.isEmpty {
                        ScrollView {
                            LazyVStack(alignment: .leading, spacing: 4) {
                                ForEach(Array(plan.issues.enumerated()), id: \.offset) { _, issue in
                                    Text(issueText(issue)).foregroundStyle(.orange).lineLimit(2)
                                        .textSelection(.enabled).help(issue.path + "\n" + issue.message)
                                }
                            }.frame(maxWidth: .infinity, alignment: .leading)
                        }.frame(maxHeight: 92)
                    }
                    Text(chinese
                        ? "已读取 \(plan.inspectedCount) / \(plan.lightCount + plan.calibrationCount) 个文件头；容量为此次检查的快照。"
                        : "Read \(plan.inspectedCount) / \(plan.lightCount + plan.calibrationCount) headers; capacity is a snapshot from this check.")
                        .foregroundStyle(.secondary)
                    Text(chinese
                        ? "覆盖分析、堆栈及后期全流程，非单独分析或复用母版的需求。按全部亮场、未裁切尺寸，含 10% 余量及 64 MiB 保留。文件头检查不代表像素解码或校准匹配已通过；实际占用可能变化。"
                        : "Covers analysis, stacking and finishing, not analysis alone or master reuse. Uses all lights and uncropped dimensions, with 10% headroom and a 64 MiB reserve. Headers do not verify pixel decoding or calibration compatibility; actual usage can vary.")
                        .foregroundStyle(.secondary)
                } else if let error, !checking, completedRequest == request {
                    Text(error).foregroundStyle(.orange).lineLimit(2)
                }
                HStack(alignment: .top) {
                    Text((chinese ? "工作目录：" : "Work directory: ") + destination.path)
                        .textSelection(.enabled).foregroundStyle(.secondary).lineLimit(2)
                    Spacer()
                    Button(chinese ? "重新检查" : "Check again") { refresh += 1 }
                        .disabled(checking || request?.recipe == nil)
                        .accessibilityIdentifier("deep-sky.preflight-refresh")
                }
            }.font(.caption).padding(.top, 6)
        } label: {
            HStack {
                Label(chinese ? "完整堆栈资源估算" : "Full workflow estimate", systemImage: needsAttention ? "exclamationmark.circle" : "checklist")
                Spacer()
                Text(summary(plan: currentPlan, request: request)).foregroundStyle(needsAttention ? .orange : .secondary).lineLimit(1)
            }
        }
        .font(.callout)
        .padding(10)
        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 8))
        .accessibilityIdentifier("deep-sky.preflight")
        .task(id: request) {
            guard let request, let recipe = request.recipe else {
                checking = false; completedRequest = nil; plan = nil; error = nil
                return
            }
            checking = true; completedRequest = nil; plan = nil; error = nil
            do {
                // Parameter edits cancel obsolete work before reading hundreds of headers.
                try await Task.sleep(for: .milliseconds(350))
                let value = try await model.inspectDeepSkyResources(recipe: recipe, destination: request.destination)
                try Task.checkCancellation()
                plan = value; completedRequest = request; checking = false
            } catch is CancellationError {
                // A replacement task owns the loading state.
            } catch {
                guard !Task.isCancelled else { return }
                self.error = chinese ? "暂时无法估算资源：\(error.localizedDescription)" : "Resource estimate unavailable: \(error.localizedDescription)"
                completedRequest = request; checking = false
            }
        }
    }

    private func summary(plan: DeepSkyResourcePlan?, request: Request?) -> String {
        guard let request else { return chinese ? "展开检查文件头与磁盘空间" : "Expand to check headers and disk space" }
        if let plan {
            if !plan.issues.isEmpty { return chinese ? "\(plan.issues.count) 项需要检查" : "\(plan.issues.count) items to review" }
            return chinese ? "规划空间约 \(bytes(plan.plannedDiskBytes)) · 可用 \(bytes(plan.availableBytes))"
                : "Plan ≈ \(bytes(plan.plannedDiskBytes)) · \(bytes(plan.availableBytes)) available"
        }
        if checking { return chinese ? "正在读取文件头…" : "Reading headers…" }
        if request.recipe == nil { return chinese ? "处理参数尚未就绪或需要修正" : "Settings are not ready or need correction" }
        return chinese ? "资源估算待检查" : "Resource estimate pending"
    }

    private func bytes(_ value: UInt64?) -> String {
        guard let value else { return chinese ? "未知" : "Unknown" }
        return ByteCountFormatter.string(fromByteCount: Int64(clamping: value), countStyle: .file)
    }
    private func metric(_ title: String, value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).foregroundStyle(.secondary)
            Text(value).monospacedDigit()
        }
    }
    private func issueText(_ issue: DeepSkyResourcePlan.Issue) -> String {
        let name = issue.path.isEmpty ? "" : URL(fileURLWithPath: issue.path).lastPathComponent + ": "
        guard chinese else { return name + issue.message }
        let text = switch issue.code {
        case "InputMissing": "尚无亮场"
        case "InputUnavailable": "文件不可读取"
        case "HeaderUnavailable": "无法读取图像尺寸"
        case "DuplicateInput": "素材在亮场或校准组中重复"
        case "GeometryMismatch": "图像尺寸不一致，请检查素材分组"
        case "DestinationUnavailable": "无法查询工作盘容量"
        case "SensorStorageUnestimated": "传感器纹理校正需要额外缓存，完整空间需求尚无法估算"
        case "EstimateOverflow": "素材规模超出可估算范围"
        default: issue.message
        }
        return name + text
    }
}

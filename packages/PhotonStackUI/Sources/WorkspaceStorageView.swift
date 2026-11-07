import Foundation
import SwiftUI

/// A read-only inventory. Cleanup stays behind the workspace's existing
/// reference-aware plan and confirmation flow.
struct WorkspaceStorageView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var snapshot: WorkspaceStorageSnapshot?
    @State private var isLoading = false
    @State private var failure: String?
    @State private var refreshGeneration = UUID()
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var refreshKey: RefreshKey {
        RefreshKey(project: model.project.id, generation: refreshGeneration)
    }

    private struct RefreshKey: Hashable {
        let project: UUID
        let generation: UUID
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Label(chinese ? "工作目录存储" : "Working directory storage", systemImage: "internaldrive")
                    .font(.callout.weight(.semibold))
                Spacer(minLength: 4)
                Button { refreshGeneration = UUID() } label: {
                    Label(chinese ? "刷新存储统计" : "Refresh storage", systemImage: "arrow.clockwise")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .disabled(isLoading)
                .help(chinese ? "只读取文件大小和引用状态，不删除文件" : "Read sizes and references without deleting files")
                .accessibilityIdentifier("workspace.storage.refresh")
            }
            if let snapshot {
                storageRow(chinese ? "总计" : "Total", bytes: snapshot.managedBytes, count: snapshot.managedFileCount)
                storageRow(chinese ? "保留" : "Retained", bytes: snapshot.protectedBytes, count: snapshot.protectedFileCount)
                storageRow(chinese ? "可清理缓存" : "Reclaimable cache", bytes: snapshot.reclaimableBytes, count: snapshot.reclaimableFileCount)
                HStack {
                    Text(chinese ? "所在磁盘可用" : "Available on volume")
                    Spacer(minLength: 6)
                    Text(snapshot.availableCapacityBytes.map(formatBytes) ?? (chinese ? "未知" : "Unknown"))
                        .monospacedDigit()
                }.font(.caption)
                Text(snapshot.rootURL.path)
                    .font(.caption2).foregroundStyle(.secondary)
                    .lineLimit(2).truncationMode(.middle).textSelection(.enabled)
                    .help(snapshot.rootURL.path)
                Button(chinese ? "检查并清理缓存…" : "Review cache cleanup…") {
                    model.requestPreviewCacheCleanup()
                }
                .disabled(isLoading || snapshot.reclaimableFileCount == 0 || model.hasActiveProcessing)
                .accessibilityIdentifier("workspace.storage.cleanup")
                Text(chinese
                     ? "保留内容包括项目引用与未知归属的文件；目录外文件不计入。清理前会重新核对引用。"
                     : "Retains project references and files of unknown ownership. Files outside this directory are not counted. References are checked again before cleanup.")
                    .font(.caption2).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if isLoading {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.mini)
                    Text(chinese ? "正在统计…" : "Measuring storage…").font(.caption)
                }
            }
            if let failure {
                Text(failure).font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .accessibilityIdentifier("workspace.storage")
        .task(id: refreshKey) { await refresh(for: refreshKey) }
        .onChange(of: model.hasActiveProcessing) { wasActive, isActive in
            if wasActive && !isActive { refreshGeneration = UUID() }
        }
        .onChange(of: model.pendingPreviewCacheCleanupPlan) { previous, current in
            if previous != nil && current == nil { refreshGeneration = UUID() }
        }
    }

    private func storageRow(_ title: String, bytes: Int64, count: Int) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(chinese ? "\(title) · \(count) 个文件" : "\(title) · \(count) files")
            Spacer(minLength: 6)
            Text(formatBytes(bytes)).monospacedDigit().fixedSize()
        }.font(.caption)
    }

    private func formatBytes(_ bytes: Int64) -> String {
        if bytes == 0 {
            return "0 KB"
        }
        return ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
    }

    @MainActor private func refresh(for key: RefreshKey) async {
        isLoading = true
        failure = nil
        snapshot = nil
        defer { if key == refreshKey { isLoading = false } }
        do {
            let measured = try await model.storageSnapshot()
            try Task.checkCancellation()
            guard key == refreshKey else { return }
            snapshot = measured
        } catch is CancellationError {
            // A different project or newer refresh owns the next presentation.
        } catch {
            guard key == refreshKey else { return }
            failure = (chinese ? "无法读取存储统计：" : "Could not measure storage: ") + error.localizedDescription
        }
    }
}

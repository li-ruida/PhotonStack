import AppKit
import SwiftUI
import PhotonStackProcessing

struct GaiaCatalogDownloadView: View {
    let source: URL
    let chinese: Bool
    let use: (URL) -> Void
    private let service: any ProcessingService = CLIProcessingService.makeDefault()
    private let downloader = GaiaSpectralDownload()
    @Environment(\.dismiss) private var dismiss
    @State private var field: GaiaSpectralField?
    @State private var query = GaiaSpectralQuery()
    @State private var result: GaiaSpectralDownload.Result?
    @State private var progress: GaiaSpectralDownload.Progress?
    @State private var operation: Task<Void, Never>?
    @State private var error: String?
    @State private var refresh = false
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    private var current: Bool { result?.manifest.field == field && result?.manifest.query == query && result != nil }
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text(t("获取 Gaia 光谱星表", "Get Gaia spectral catalog")).font(.headline)
                Spacer()
                Button(t("打开已保存数据…", "Open saved data…")) { reopen() }.disabled(operation != nil || field == nil).accessibilityIdentifier("gaia.reopen")
                ToolSheetCloseButton(chinese: chinese, busy: operation != nil) { dismiss() }
            }
            Text(source.lastPathComponent).font(.caption).foregroundStyle(.secondary)
            if let field {
                Text(String(format: t("ICRS 中心 %.5f° / %+.5f° · 视场包围半径 %.3f°", "ICRS center %.5f° / %+.5f° · Field bounding radius %.3f°"), field.ra, field.dec, field.radius)).font(.caption).monospacedDigit()
            } else { Text(t("读取线性母片与天球坐标…", "Reading the linear master and WCS…")).font(.caption) }
            Form {
                TextField(t("拍摄时间（UTC）", "Observation time (UTC)"), text: $query.observationUTC, prompt: Text("YYYY-MM-DDTHH:mm:ss"))
                    .textFieldStyle(.roundedBorder).accessibilityIdentifier("gaia.date")
                Text(t("填写拍摄时间（UTC），用于修正星位。只填日期时按当天 00:00 计算。", "Enter a representative observation time for this master. A date alone means midnight UTC. It propagates J2016.0 positions to the observation epoch; dates are not inferred from filenames or today's date.")).font(.caption).foregroundStyle(.secondary)
                HStack {
                    TextField(t("最亮 G 星等", "Brightest G magnitude"), value: $query.minimumMagnitude, format: .number).frame(width: 190).accessibilityIdentifier("gaia.minimum")
                    TextField(t("最暗 G 星等", "Faintest G magnitude"), value: $query.maximumMagnitude, format: .number).frame(width: 190).accessibilityIdentifier("gaia.maximum")
                }
                Stepper(t("候选上限：\(query.maximumStars)", "Candidate limit: \(query.maximumStars)"), value: $query.maximumStars, in: 20...1000, step: 20)
                    .accessibilityIdentifier("gaia.limit")
                Toggle(t("重新联网获取（不复用缓存）", "Download fresh data (bypass cache)"), isOn: $refresh).accessibilityIdentifier("gaia.refresh")
            }.formStyle(.grouped).disabled(operation != nil)
            Text(t("向 Gaia 查询星表和光谱，照片不上传。下载后可离线使用。", "Queries send field coordinates, magnitude limits and source IDs to ESA Gaia. Photos remain local. Original tables, spectra, query parameters and digests are saved together for offline reuse.")).font(.caption).foregroundStyle(.secondary)
            if let result {
                VStack(alignment: .leading, spacing: 5) {
                    Text(t("已保存 \(result.manifest.count) 颗光谱星 · 跳过 \(result.manifest.skipped.count) 颗", "Saved \(result.manifest.count) spectral stars · Skipped \(result.manifest.skipped.count)")).font(.subheadline.bold())
                    Text(String(format: "J%.8f · %d × 336–1020 nm", result.manifest.query.epoch ?? 0, 343)).font(.caption).monospacedDigit()
                    if result.manifest.limited { Text(t("达到候选数量上限，仅选取当前范围内较亮的恒星。可提高上限后重新下载。", "Candidate limit reached; this contains the brightest subset. Increase the limit to retrieve more.")).font(.caption).foregroundStyle(.orange) }
                    if !result.manifest.skipped.isEmpty {
                        DisclosureGroup(t("跳过原因", "Skipped sources")) {
                            ScrollView { VStack(alignment: .leading) { ForEach(result.manifest.skipped, id: \.id) { s in Text(s.id + " · " + reason(s.reason)).font(.caption).textSelection(.enabled) } } }.frame(maxHeight: 80)
                        }.font(.caption)
                    }
                    Button(t("查看数据文件夹", "Show data folder")) { NSWorkspace.shared.activateFileViewerSelecting([result.directory]) }
                    if !current { Text(t("查询条件已改变，请重新下载或重开保存的数据。", "Query settings changed; download again or reopen saved data.")).font(.caption).foregroundStyle(.orange) }
                }
            }
            Spacer(minLength: 0)
            if let error { ProcessingErrorNotice(message:error,chinese:chinese) }
            ToolActionBar {
                if operation != nil {
                    ProgressView().controlSize(.small)
                    if let progress {
                        VStack(alignment: .leading) {
                            Text(t("请求 \(progress.completed)/\(progress.total) · 缓存 \(progress.cacheHits)", "Requests \(progress.completed)/\(progress.total) · Cached \(progress.cacheHits)"))
                            Text(t("已接收 \(progress.receivedBytes / 1024) KB · 重试 \(progress.retries)", "Received \(progress.receivedBytes / 1024) KB · Retries \(progress.retries)"))
                        }.font(.caption).monospacedDigit()
                    }
                    Button(t("取消任务", "Cancel task")) { operation?.cancel() }.keyboardShortcut(".", modifiers: .command).accessibilityIdentifier("gaia.cancel")
                }
                Spacer()
                Button(t("查询并保存…", "Query & save…")) { download() }.disabled(operation != nil || field == nil || !query.valid).accessibilityIdentifier("gaia.download")
                Button(t("使用此星表", "Use catalog")) {
                    guard let result, let field else { return }
                    perform { let checked = try await downloader.reopen(result.directory, source: source, field: field); use(checked.directory.appendingPathComponent("catalog.txt")); dismiss() }
                }.toolPrimaryAction().disabled(operation != nil || !current).accessibilityIdentifier("gaia.use")
            }
        }.padding(18).frame(width: 760, height: 680)
            .interactiveDismissDisabled(operation != nil)
            .task { perform { field = try await GaiaSpectralField.read(source, service: service) } }
            .onDisappear { operation?.cancel() }
    }
    private func reason(_ value: String) -> String {
        switch value {
        case "proper-motion-missing": t("缺少自行", "Missing proper motion")
        case "proper-motion-outside-query-margin": t("自行超出查询边界约定", "Proper motion exceeds query margin")
        default: t("未返回光谱", "Spectrum not returned")
        }
    }
    private func perform(_ action: @escaping @MainActor () async throws -> Void) {
        guard operation == nil else { return }; error = nil
        operation = Task { defer { operation = nil }; do { try await action() } catch is CancellationError {} catch {
            if !Task.isCancelled { self.error = error.localizedDescription }
        } }
    }
    private func folder(_ message: String) -> URL? {
        let p = NSOpenPanel(); p.canChooseFiles = false; p.canChooseDirectories = true; p.canCreateDirectories = true; p.message = message
        return p.runModal() == .OK ? p.url : nil
    }
    private func download() {
        guard let field, let parent = folder(t("选择数据保存位置，将创建新的 Gaia 数据文件夹。", "Choose a location for a new Gaia data folder.")) else { return }
        let destination = parent.appendingPathComponent("GaiaCatalog-" + UUID().uuidString.prefix(8)), requested = query
        perform {
            result = try await downloader.download(source: source, field: field, query: requested, to: destination, refresh: refresh) { value in
                await MainActor.run { progress = value }
            }
        }
    }
    private func reopen() {
        guard let field, let directory = folder(t("选择包含 query.json 的 Gaia 数据文件夹。", "Choose a Gaia data folder containing query.json.")) else { return }
        perform { let loaded = try await downloader.reopen(directory, source: source, field: field); result = loaded; query = loaded.manifest.query }
    }
}

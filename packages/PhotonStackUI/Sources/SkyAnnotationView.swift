import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct SkyAnnotationControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var presented = false
    var body: some View {
        Button(model.language == .simplifiedChinese ? "天体标注与星表…" : "Deep-sky annotations…") { presented = true }
            .disabled(model.currentInputURL == nil || model.hasActiveProcessing).accessibilityIdentifier("sky-annotation.open")
            .sheet(isPresented: $presented) { if let source = model.currentInputURL { SkyAnnotationView(source: source, chinese: model.language == .simplifiedChinese) } }
    }
}
struct SkyAnnotationView: View {
    @StateObject private var session: SkyAnnotationSession
    let chinese: Bool
    @Environment(\.dismiss) private var dismiss
    @State private var selected: String?
    @State private var actualSize = false
    @State private var operation: Task<Void, Never>?
    @State private var savedURL: URL?
    init(source: URL, chinese: Bool) { self.chinese = chinese; _session = StateObject(wrappedValue: SkyAnnotationSession(source: source)) }
    private func text(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Text(text("天体标注", "Deep-sky annotations")).font(.headline)
                Spacer()
                Button(text("选择带坐标母片…", "Choose solved master…")) { choose() }.disabled(session.busy || operation != nil)
                Button(text("打开结果…", "Open result…")) { open() }.disabled(session.busy || operation != nil).accessibilityIdentifier("sky-annotation.reopen")
                Button(text("关闭", "Close")) { dismiss() }.keyboardShortcut(.cancelAction)
            }
            HStack(alignment: .top, spacing: 14) {
                VStack {
                    GeometryReader { geometry in
                        if let report = session.report, let image = session.image {
                            let scale = actualSize ? 1 : min(geometry.size.width/Double(report.previewWidth), geometry.size.height/Double(report.previewHeight+40))
                            ScrollView([.horizontal, .vertical]) {
                                SkyAnnotationCanvas(report: report, image: image, selected: selected)
                                    .frame(width: Double(report.previewWidth)*scale, height: Double(report.previewHeight+40)*scale)
                            }
                        } else {
                            ContentUnavailableView(text("为星云与星系标出名称", "Identify nebulae and galaxies"), systemImage: "sparkle.magnifyingglass", description: Text(text("选择带 WCS 的 FITS 母片，然后生成标注。可先使用天文定位取得坐标。", "Choose a WCS FITS master and generate annotations. Plate solve first if coordinates are missing.")))
                        }
                    }
                    HStack { Toggle(text("100% 查看", "View at 100%"), isOn: $actualSize).toggleStyle(.button); Spacer(); Text(session.source.lastPathComponent).font(.caption).lineLimit(1) }
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
                VStack(alignment: .leading, spacing: 10) {
                    filters.disabled(session.busy || operation != nil)
                    if let r = session.report {
                        Text(text("显示 \(r.objects.count) / \(r.matched) 个目标", "Showing \(r.objects.count) of \(r.matched) objects")).font(.caption)
                        if r.showLabels && r.objects.filter(\.labelVisible).count < r.objects.count {
                            Text(text("部分名称因重叠隐藏，完整目标可在下方列表查看。", "Some labels are hidden to avoid overlap; all selected objects remain in the list.")).font(.caption2).foregroundStyle(.secondary)
                        }
                        List(r.objects, selection: $selected) { object in
                            VStack(alignment: .leading) { Text(object.label); if !object.commonNames.isEmpty { Text(object.commonNames).font(.caption).foregroundStyle(.secondary).lineLimit(1) } }.tag(object.id)
                        }.listStyle(.bordered).accessibilityIdentifier("sky-annotation.objects")
                        if let object = r.objects.first(where: { $0.id == selected }) { detail(object) }
                        Text(r.linearDisplayPreview ? text("线性母片使用显示拉伸预览；未改原像素。", "Linear master shown with a display stretch; source pixels are unchanged.") : text("保留母片已有的显示色彩。", "Preserves the master's display colors.")).font(.caption2).foregroundStyle(.secondary)
                    }
                    if !session.isCurrent, session.report != nil { Text(text("筛选或显示设置已变更，请更新标注后导出。", "Settings changed; update annotations before export.")).font(.caption).foregroundStyle(.orange) }
                    if let error = session.error { ProcessingErrorNotice(message:error,chinese:chinese) }
                }.frame(width: 320)
            }
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text(text("OpenNGC · 离线星表 · CC BY-SA 4.0", "OpenNGC · offline catalog · CC BY-SA 4.0")).font(.caption)
                    Text(text("轮廓与尺寸仅作定位示意，不代表从照片测得的星云边界。", "Catalog outlines and sizes are approximate, not boundaries measured in this image.")).font(.caption2).foregroundStyle(.secondary)
                }
                Spacer()
                if session.busy { ProgressView().controlSize(.small); Button(text("取消", "Cancel")) { session.cancel(); operation?.cancel() } }
                if let savedURL { Button(text("查看导出", "Show export")) { NSWorkspace.shared.activateFileViewerSelecting([savedURL]) } }
                Button(text("导出标注…", "Export annotations…")) { save() }.disabled(session.busy || !session.isCurrent || operation != nil).buttonStyle(.borderedProminent).accessibilityIdentifier("sky-annotation.save")
            }
        }.padding(18).frame(width: 1100, height: 750)
        .onDisappear { let pending = operation; pending?.cancel(); Task { await pending?.value; session.close() } }
        .onChange(of: session.report?.sourceSHA) { _, _ in selected = nil }
    }
    private var filters: some View {
        VStack(alignment: .leading, spacing: 8) {
            TextField(text("搜索名称 / NGC / IC / M 编号", "Search names / NGC / IC / M"), text: $session.settings.query).textFieldStyle(.roundedBorder).accessibilityIdentifier("sky-annotation.query").onSubmit { session.begin() }
            Picker(text("类别", "Category"), selection: $session.settings.category) {
                Text(text("全部", "All")).tag("all"); Text(text("星云", "Nebulae")).tag("nebulae"); Text(text("星系", "Galaxies")).tag("galaxies"); Text(text("星团", "Clusters")).tag("clusters"); Text(text("其他", "Other")).tag("other")
            }
            HStack { Text(text("最暗星等", "Limiting magnitude")); TextField("", value: $session.settings.limitingMagnitude, format: .number.precision(.fractionLength(1))).frame(width: 60) }.font(.caption)
            Toggle(text("包含无星等记录的目标", "Include unknown magnitudes"), isOn: $session.settings.includeUnknownMagnitude).font(.caption)
            Stepper(text("最多 \(session.settings.maxObjects) 个", "Up to \(session.settings.maxObjects) objects"), value: $session.settings.maxObjects, in: 1...1000, step: 25).font(.caption)
            HStack { Toggle(text("轮廓", "Outlines"), isOn: $session.settings.showOutlines); Toggle(text("名称", "Labels"), isOn: $session.settings.showLabels) }.font(.caption)
            Button(text("生成 / 更新标注", "Generate / update annotations")) { session.begin() }.disabled(!session.settings.valid).accessibilityIdentifier("sky-annotation.run")
        }
    }
    private func detail(_ o: SkyAnnotationReport.Object) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(o.label).font(.subheadline.bold())
            Text(String(format: "RA %.6f° · Dec %+.6f°", o.ra, o.dec)).font(.caption).monospacedDigit()
            Text(String(format: "%.2f′ × %.2f′", o.majorArcmin, o.minorArcmin)).font(.caption)
            if let magnitude = o.magnitude { Text(String(format: "%@ %.2f mag", o.magnitudeBand, magnitude)).font(.caption) }
            Text(o.outlineKind == "published-contour" ? text("公开星云轮廓，可能覆盖整个复合区。", "Published contour; may cover a larger complex.") : o.outlineKind == "catalog-ellipse" ? text("按星表尺寸与方向绘制的示意椭圆。", "Ellipse from catalog size and orientation.") : text("尺寸或方向不足，仅标出中心。", "Insufficient shape metadata; center only.")).font(.caption2).foregroundStyle(.secondary)
            if o.invalidPositionAngle { Text(text("星表方向角无效，已忽略。", "Invalid catalog position angle ignored.")).font(.caption2).foregroundStyle(.orange) }
        }.textSelection(.enabled)
    }
    private func choose() { let p = NSOpenPanel(); p.allowedContentTypes = [UTType(filenameExtension: "fits") ?? .data, UTType(filenameExtension: "fit") ?? .data]; if p.runModal() == .OK, let url = p.url { session.choose(url); selected = nil } }
    private func folder() -> URL? { let p = NSOpenPanel(); p.canChooseFiles = false; p.canChooseDirectories = true; p.canCreateDirectories = true; return p.runModal() == .OK ? p.url : nil }
    private func open() { guard let folder = folder() else { return }; operation = Task { defer { operation = nil }; do { try await session.open(folder); session.error = nil; selected = nil } catch is CancellationError { } catch { if !Task.isCancelled { session.error = error.localizedDescription } } } }
    private func save() { guard let parent = folder() else { return }; operation = Task { defer { operation = nil }; do { savedURL = try await session.save(to: parent.appendingPathComponent("SkyAnnotation-"+UUID().uuidString.prefix(8))); session.error = nil } catch is CancellationError { } catch { if !Task.isCancelled { session.error = error.localizedDescription } } } }
}

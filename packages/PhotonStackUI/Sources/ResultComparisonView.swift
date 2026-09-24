import AppKit
import PhotonStackAppCore
import SwiftUI
import UniformTypeIdentifiers

struct ResultComparisonControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    var context = "workspace"
    @State private var presented = false
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var currentResult: URL? {
        context == "deep-sky" ? model.latestDeepSkyResultURL : (model.previewURL ?? model.latestDeepSkyResultURL)
    }

    var body: some View {
        Button { presented = true } label: {
            Label(chinese ? "成片对比" : "Compare results", systemImage: "rectangle.on.rectangle")
        }
        .accessibilityIdentifier(context == "workspace" ? "result-comparison.open" : "result-comparison.open.\(context)")
        .help(chinese ? "在同一位置和缩放下切换成片，检查细节与噪点。" : "Compare detail and noise at the same position and zoom.")
        .disabled(model.isProcessing)
        .sheet(isPresented: $presented) {
            ResultComparisonView(currentURL: currentResult,
                                 referenceURL: previousResult, chinese: chinese, historySources: historySources)
        }
    }

    private var previousResult: URL? {
        let current = currentResult
        if let input = ResultComparisonHistory.finishingInput(current: current,
                                                               operations: model.project.editGraph.operations) {
            return input
        }
        // Only suggest an earlier result from this project's deep-sky history.
        // Arbitrary loaded photographs must be selected explicitly.
        guard current == model.latestDeepSkyResultURL else { return nil }
        return model.project.editGraph.operations.reversed().compactMap { operation -> URL? in
            guard operation.isEnabled, operation.parameters["mode"] == "deepSkyRecipe",
                  let path = operation.parameters["output"], !path.isEmpty else { return nil }
            let url = URL(fileURLWithPath: path)
            return url != current && FileManager.default.fileExists(atPath: path) ? url : nil
        }.first
    }

    private var historySources: ResultComparisonSources? {
        guard let current = currentResult, let reference = previousResult else { return nil }
        func report(_ output: URL) -> URL? {
            guard let operation = model.project.editGraph.operations.last(where: {
                $0.isEnabled && $0.parameters["mode"] == "deepSkyRecipe" && $0.parameters["output"] == output.path
            }), let path = operation.parameters["report"] else { return nil }
            return URL(fileURLWithPath: path)
        }
        guard let a = report(current), let b = report(reference) else { return nil }
        return ResultComparisonSources(currentReport: a, referenceReport: b)
    }
}

/// Suggest the actual input of a known, geometry-preserving display adjustment.
/// Matching the output matters: the last history entry may belong to another branch.
enum ResultComparisonHistory {
    static func finishingInput(current: URL?, operations: [EditOperation]) -> URL? {
        guard let current,
              let operation = operations.last(where: { $0.parameters["output"] == current.path }),
              operation.isEnabled else { return nil }
        let mode = operation.parameters["mode"]
        let supported = (operation.kind == .localContrast && mode == "continuum-v1")
            || (operation.kind == .denoise && mode == "display-grid-v1")
            || (operation.kind == .colorNeutralize && mode == "removeGreen")
        guard supported, let path = operation.parameters["input"], !path.isEmpty else { return nil }
        let input = URL(fileURLWithPath: path)
        let displayExtensions: Set<String> = ["tif", "tiff", "png", "jpg", "jpeg", "heic", "heif"]
        guard input.standardizedFileURL != current.standardizedFileURL,
              displayExtensions.contains(input.pathExtension.lowercased()),
              displayExtensions.contains(current.pathExtension.lowercased()),
              FileManager.default.fileExists(atPath: input.path),
              FileManager.default.fileExists(atPath: current.path) else { return nil }
        return input
    }
}

struct ResultComparisonPair {
    let current: CGImage
    let reference: CGImage
    static func validate(current: CGImage, reference: CGImage) -> Bool {
        current.width == reference.width && current.height == reference.height
    }
}

struct ResultComparisonView: View {
    @Environment(\.dismiss) private var dismiss
    @State var currentURL: URL?
    @State var referenceURL: URL?
    let chinese: Bool
    @State var historySources: ResultComparisonSources? = nil
    @State private var pair: ResultComparisonPair?
    @State private var error: String?
    @State private var loading = false
    @State private var loadRevision = 0
    @State private var showReference = false
    @State private var magnification = 1.0
    @State private var fitToWindow = true
    @State private var commonRegion: CGRect?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(chinese ? "成片对比" : "Compare results").font(.title2)
                Spacer()
                Button(chinese ? "关闭" : "Close") { dismiss() }.keyboardShortcut(.cancelAction)
            }
            HStack {
                chooseButton(reference: true)
                chooseButton(reference: false)
                Spacer()
                Button(chinese ? "适配" : "Fit") { fitToWindow = true }
                    .accessibilityIdentifier("result-comparison.fit")
                Button("1:1") { fitToWindow = false; magnification = 1 }
                    .accessibilityIdentifier("result-comparison.native")
                Button("2×") { fitToWindow = false; magnification = 2 }
                    .accessibilityIdentifier("result-comparison.double")
                Text("\(Int((magnification * 100).rounded()))%")
                    .monospacedDigit().frame(width: 55)
            }
            HStack {
                Picker(chinese ? "显示" : "Show", selection: $showReference) {
                    Text(chinese ? "参考成片" : "Reference").tag(true)
                    Text(chinese ? "当前成片" : "Current").tag(false)
                }.pickerStyle(.segmented).frame(width: 280)
                Button(chinese ? "切换（空格）" : "Toggle (Space)") { showReference.toggle() }
                    .keyboardShortcut(.space, modifiers: [])
                    .accessibilityIdentifier("result-comparison.toggle")
                Spacer()
                if let pair { Text("\(pair.current.width) × \(pair.current.height) px").foregroundStyle(.secondary) }
            }.disabled(pair == nil || loading)
            HStack {
                Text((chinese ? "参考：" : "Reference: ") + (referenceURL?.lastPathComponent ?? "—"))
                    .help(referenceURL?.path ?? "")
                Spacer()
                Text((chinese ? "当前：" : "Current: ") + (currentURL?.lastPathComponent ?? "—"))
                    .help(currentURL?.path ?? "")
            }.font(.caption).foregroundStyle(.secondary).lineLimit(1)
            if let region = commonRegion {
                Text(chinese ? "同一母片的共同区域：\(Int(region.width)) × \(Int(region.height)) px；原像素对比，未缩放配准。"
                     : "Shared master region: \(Int(region.width)) × \(Int(region.height)) px; original pixels without resampling.")
                    .font(.caption).foregroundStyle(.secondary)
                    .accessibilityIdentifier("result-comparison.common-region")
            }
            ZStack {
                if let pair, let currentURL, let referenceURL {
                    NativeImagePreview(url: showReference ? referenceURL : currentURL,
                                       magnification: $magnification, fitToWindow: $fitToWindow,
                                       decodedImage: showReference ? pair.reference : pair.current,
                                       centerFirstImage: true)
                } else if loading {
                    ProgressView(chinese ? "正在读取原生像素…" : "Loading native pixels…")
                } else {
                    ContentUnavailableView(chinese ? "选择两张成片" : "Choose two results",
                                           systemImage: "rectangle.on.rectangle",
                                           description: Text(error ?? (chinese ? "使用相同构图、相同尺寸的 TIFF、PNG、JPEG 或 HEIF 成片。" : "Choose TIFF, PNG, JPEG or HEIF images with identical framing and dimensions.")))
                }
            }.frame(maxWidth: .infinity, maxHeight: .infinity)
            Text(chinese ? "切换时保持位置与缩放。比较环状边缘、暗纹和背景颗粒；此窗口不会改变项目或导出结果。" : "Position and zoom stay fixed while switching. Inspect edges, dust lanes and grain. This view does not change the project or exported result.")
                .font(.caption).foregroundStyle(.secondary)
        }.padding(16).frame(width: 1060, height: 720)
            .task(id: loadRevision) { await loadPair() }
    }

    private func chooseButton(reference: Bool) -> some View {
        let url = reference ? referenceURL : currentURL
        let label = reference ? (chinese ? "选择参考图…" : "Choose reference…") : (chinese ? "选择当前图…" : "Choose current…")
        return Button(label) {
            let panel = NSOpenPanel()
            panel.allowedContentTypes = [.tiff, .png, .jpeg, .heic]
            panel.allowsMultipleSelection = false
            panel.message = chinese ? "选择相同构图的已显影成片。" : "Select a developed image with matching framing."
            if panel.runModal() == .OK, let chosen = panel.url {
                // Clear both decoded images before any new file can be labelled.
                pair = nil; error = nil; commonRegion = nil; historySources = nil
                if reference { referenceURL = chosen } else { currentURL = chosen }
                loadRevision += 1
            }
        }.help(url?.path ?? label)
            .accessibilityIdentifier(reference ? "result-comparison.choose-reference" : "result-comparison.choose-current")
    }

    @MainActor private func loadPair() async {
        pair = nil; error = nil; loading = false; commonRegion = nil
        guard let currentURL, let referenceURL else { return }
        guard currentURL.standardizedFileURL != referenceURL.standardizedFileURL else {
            error = chinese ? "请选择两张不同的成片。" : "Choose two different image files."; return
        }
        loading = true
        do {
            let loader = NativePreviewImageLoader()
            guard let current = try await loader.load(currentURL), let reference = try await loader.load(referenceURL) else {
                throw CocoaError(.fileReadCorruptFile)
            }
            try Task.checkCancellation()
            if let historySources {
                let geometry = try await ResultComparisonProvenanceLoader().load(historySources,
                    currentURL: currentURL, referenceURL: referenceURL,
                    currentSize: CGSize(width: current.width, height: current.height),
                    referenceSize: CGSize(width: reference.width, height: reference.height))
                try Task.checkCancellation()
                pair = try geometry.crop(current: current, reference: reference)
                commonRegion = geometry.common
                fitToWindow = false; magnification = 1; showReference = false; loading = false
                return
            }
            guard ResultComparisonPair.validate(current: current, reference: reference) else {
                error = chinese ? "尺寸不同：当前图 \(current.width)×\(current.height)，参考图 \(reference.width)×\(reference.height)。请使用同一裁边范围的成片，避免缩放造成误判。" : "Dimensions differ: current \(current.width)×\(current.height), reference \(reference.width)×\(reference.height). Use the same crop for a fair comparison."
                loading = false; return
            }
            pair = ResultComparisonPair(current: current, reference: reference)
            fitToWindow = true; showReference = false; loading = false
        } catch is CancellationError { return }
        catch let issue as ResultComparisonGeometryError {
            guard !Task.isCancelled else { return }
            switch issue {
            case .unavailable:
                error = chinese ? "旧成片缺少母片来源或裁边记录，无法自动核对位置。可手动选择两张已确认构图一致的成片。"
                    : "A result lacks master identity or crop metadata. Manually choose images whose framing you have verified."
            case .unrelated:
                error = chinese ? "两张成片来自不同母片，无法自动保证位置一致。请手动选择并确认构图。"
                    : "These results use different masters. Choose images manually after checking their framing."
            case .inconsistent:
                error = chinese ? "成片尺寸与裁边记录不一致，无法安全对齐。请检查所选文件。"
                    : "Image dimensions do not match the crop record. Check the selected files."
            case .noOverlap:
                error = chinese ? "两张成片的裁边没有共同区域。" : "The two crops have no overlapping region."
            case .changedOutput:
                error = chinese ? "成片文件在处理完成后被修改，原裁边记录可能已失效。请手动选择并确认构图。"
                    : "An image was changed after processing. Its crop record may be stale; choose and verify the framing manually."
            }
            loading = false
        }
        catch {
            guard !Task.isCancelled else { return }
            self.error = chinese ? "无法读取成片，请检查文件是否仍然存在。" : "Cannot read the image. Check that the file still exists."
            loading = false
        }
    }
}

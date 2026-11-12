import PhotonStackAppCore
import PhotonStackProcessing
import SwiftUI

/// Published only to the canvas. Dragging sliders doesn't invalidate the workspace
/// or substitute a preview bitmap for the input to another edit or export.
@MainActor
final class PhotoAdjustmentPreview: ObservableObject {
    struct Frame {
        let input: URL
        let settings: PhotoAdjustments
        let image: CGImage
        let sourceSize: CGSize
    }
    @Published private(set) var frame: Frame?
    @Published private(set) var errorMessage: String?
    private var pending: (URL, PhotoAdjustments, PhotoClippingWarnings)?
    private var task: Task<Void, Never>?
    private var renderer: PhotoAdjustmentRenderer?

    func schedule(input: URL, settings: PhotoAdjustments, warnings: PhotoClippingWarnings = []) {
        pending = (input, settings, warnings)
        guard task == nil else { return }
        let renderer = renderer ?? PhotoAdjustmentRenderer()
        self.renderer = renderer
        task = Task {
            // Consume the most recent slider state per frame. Only one render
            // is active even when pointer events arrive faster than the GPU.
            while !Task.isCancelled, pending != nil {
                do {
                    guard let (source, values, warnings) = pending else { break }
                    pending = nil
                    let preview = try await renderer.preview(input: source, adjustments: values, warnings: warnings)
                    try Task.checkCancellation()
                    frame = Frame(input: source, settings: values, image: preview.image, sourceSize: preview.sourceSize)
                    if errorMessage != nil { errorMessage = nil }
                } catch {
                    guard !Task.isCancelled else { return }
                    frame = nil
                    errorMessage = error.localizedDescription
                }
            }
            if !Task.isCancelled { task = nil }
        }
    }

    func clear(releaseSource: Bool = false) {
        task?.cancel()
        task = nil
        pending = nil
        if frame != nil { frame = nil }
        if errorMessage != nil { errorMessage = nil }
        if releaseSource { renderer = nil }
    }
}

@MainActor
final class PhotoAdjustmentDraft: ObservableObject {
    @Published var settings = PhotoAdjustments()
    @Published private(set) var picking = false
    @Published private(set) var sampleRejected = false
    @Published private(set) var sampleError: String?
    private var sampleTask: Task<Void, Never>?

    func togglePicker() {
        let wasPicking = picking
        cancelPicker()
        picking = !wasPicking
    }

    func cancelPicker() {
        sampleTask?.cancel()
        sampleTask = nil
        if picking { picking = false }
        if sampleRejected { sampleRejected = false }
        if sampleError != nil { sampleError = nil }
    }

    func sample(input: URL, point: CGPoint, displayed: PhotoAdjustments) {
        guard picking, displayed == settings else { return }
        sampleTask?.cancel()
        sampleTask = Task {
            do {
                let sample = try await PhotoAdjustmentRenderer().neutralSample(input: input, adjustments: displayed, at: point)
                guard !Task.isCancelled, picking, displayed == settings else { return }
                guard let sample else {
                    sampleRejected = true
                    return
                }
                var next = settings
                next.neutralSample = sample
                next[.warmth] = 0
                next[.tint] = 0
                settings = next
                cancelPicker()
            } catch {
                guard !Task.isCancelled else { return }
                sampleError = error.localizedDescription
            }
        }
    }
}

struct PhotoDraftNotice: View {
    @ObservedObject var draft: PhotoAdjustmentDraft
    let chinese: Bool
    let resume: () -> Void

    var body: some View {
        if !draft.settings.isNeutral {
            Button(chinese ? "继续未应用的调整" : "Resume unapplied edits", action: resume)
                .buttonStyle(.link)
        }
    }
}

struct PhotoAdjustmentEditor: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let input: URL
    @ObservedObject var draft: PhotoAdjustmentDraft
    let composition: Bool
    @State private var sourceSize: CGSize?
    @State private var showBefore = false
    @State private var warnShadows = false
    @State private var warnHighlights = false
    private var warnings: PhotoClippingWarnings {
        var warnings = PhotoClippingWarnings()
        if !composition && warnShadows { warnings.insert(.shadows) }
        if !composition && warnHighlights { warnings.insert(.highlights) }
        return warnings
    }
    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if PhotoAdjustments.supports(input) {
                HStack {
                    Button(chinese ? "重置" : "Reset") { draft.cancelPicker(); draft.settings = PhotoAdjustments() }
                        .buttonStyle(.borderless)
                    Toggle(chinese ? "看原效果" : "Before", isOn: $showBefore)
                        .toggleStyle(.button)
                    Spacer()
                    Button(chinese ? "应用调整" : "Apply edits") {
                        model.startPhotoAdjustment(draft.settings, input: input)
                    }
                    .buttonStyle(.borderedProminent)
                    .accessibilityIdentifier("finish.apply")
                }
                .disabled(draft.settings.isNeutral)
                if composition {
                    Picker(chinese ? "裁切比例" : "Aspect ratio", selection: $draft.settings.aspect) {
                        ForEach(PhotoAdjustments.Aspect.allCases, id: \.self) { aspect in
                            Text(aspect == .original ? (chinese ? "原始比例" : "Original") : aspect.rawValue).tag(aspect)
                        }
                    }
                    HStack {
                        Button { draft.settings.rotate(clockwise: false) } label: {
                            Label(chinese ? "左转" : "Left", systemImage: "rotate.left")
                        }
                        Button { draft.settings.rotate(clockwise: true) } label: {
                            Label(chinese ? "右转" : "Right", systemImage: "rotate.right")
                        }
                    }
                    HStack {
                        Toggle(chinese ? "水平翻转" : "Flip horizontally", isOn: $draft.settings.mirror)
                            .toggleStyle(.button)
                        Toggle(chinese ? "垂直翻转" : "Flip vertically", isOn: $draft.settings.flipVertical)
                            .toggleStyle(.button)
                    }
                    ParameterSlider(title: chinese ? "裁切缩放" : "Crop zoom",
                        value: $draft.settings.cropScale, range: 1...3, continuous: true)
                    if let sourceSize {
                        let size = draft.settings.quarterTurns.isMultiple(of: 2) ? sourceSize
                            : CGSize(width: sourceSize.height, height: sourceSize.width)
                        let crop = draft.settings.cropRect(in: size)
                        if crop.width < size.width {
                            ParameterSlider(title: chinese ? "水平取景" : "Horizontal framing",
                                value: $draft.settings.horizontalPosition, range: 0...1, continuous: true)
                        }
                        if crop.height < size.height {
                            ParameterSlider(title: chinese ? "垂直取景" : "Vertical framing",
                                value: $draft.settings.verticalPosition, range: 0...1, continuous: true)
                        }
                        Text("\(Int(crop.width)) × \(Int(crop.height)) px")
                            .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                    }
                } else {
                    sectionTitle(chinese ? "明暗" : "Light")
                    adjustmentRows([.exposure, .contrast, .highlights, .shadows, .blackPoint, .whitePoint])
                    HStack {
                        Toggle(chinese ? "欠曝（蓝）" : "Shadows (blue)", isOn: $warnShadows)
                            .accessibilityIdentifier("finish.warn-shadows")
                            .help(chinese ? "三个通道均达到黑场" : "All three channels clip to black")
                        Toggle(chinese ? "过曝（红）" : "Highlights (red)", isOn: $warnHighlights)
                            .accessibilityIdentifier("finish.warn-highlights")
                            .help(chinese ? "任一通道达到白场" : "Any channel clips to white")
                    }.toggleStyle(.checkbox)
                    sectionTitle(chinese ? "色彩" : "Color")
                    HStack {
                        Button {
                            showBefore = false
                            draft.togglePicker()
                        } label: {
                            Label(draft.picking ? (chinese ? "取消取样" : "Cancel sampling")
                                  : (chinese ? "取样白平衡" : "Sample white balance"), systemImage: "eyedropper")
                        }
                        .accessibilityIdentifier("finish.white-balance-picker")
                        if draft.settings.neutralSample != nil {
                            Button(chinese ? "清除取样" : "Clear sample") {
                                draft.cancelPicker()
                                draft.settings.neutralSample = nil
                            }
                        }
                    }
                    if let sample = draft.settings.neutralSample {
                        let factors = [sample.red, sample.green, sample.blue].map {
                            (sample.luminance / $0).formatted(.number.precision(.fractionLength(3)))
                        }.joined(separator: " / ")
                        Text((chinese ? "取样增益 R/G/B  " : "Sample gains R/G/B  ") + factors)
                            .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                    }
                    if draft.picking {
                        let prompt = draft.sampleRejected
                            ? (chinese ? "该处过暗、已剪切或透明，请重选中性区域。" : "Sample too dark, clipped or transparent. Choose another neutral area.")
                            : (chinese ? "请点画布中的中性灰区域（5 × 5 像素）。" : "Click a neutral gray area on the canvas (5 × 5 pixels).")
                        Text(draft.sampleError ?? prompt).font(.caption)
                    }
                    adjustmentRows([.warmth, .tint, .saturation, .vibrance])
                }
                if !draft.settings.isNeutral {
                    Text(chinese ? "当前为试调，应用后可导出。" : "Previewing edits. Apply to include them in export.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                PhotoPreviewError(preview: model.photoPreview)
            } else if AssetKind.detect(from: input) == .fits {
                Text(chinese ? "先将线性 FITS 显影，再调整明暗和色彩。" : "Develop the linear FITS to adjust light and color.")
                    .font(.callout)
                AstroDevelopControl(model: model)
            } else if AssetKind.detect(from: input) == .raw, input == model.selectedAsset?.originalURL {
                Button(chinese ? "解码 RAW 后调整" : "Decode RAW to edit") { model.prepareRawForFinishing() }
                    .disabled(model.isRawPreviewUpdating)
                if model.isRawPreviewUpdating { ProgressView().controlSize(.small) }
            } else {
                Text(chinese ? "请选择已显影的图像。" : "Select a developed image.")
            }
        }
        .onChange(of: PreviewRequest(input: input, settings: draft.settings, showBefore: showBefore, warnings: warnings, picking: draft.picking), initial: true) { _, request in
            model.markPhotoDraft(for: request.settings.isNeutral ? nil : request.input)
            if (request.settings.isNeutral && request.warnings.isEmpty && !request.picking) || request.showBefore || !PhotoAdjustments.supports(request.input) {
                model.photoPreview.clear()
            } else {
                model.photoPreview.schedule(input: request.input, settings: request.settings, warnings: request.warnings)
            }
        }
        .task(id: composition) {
            if composition, sourceSize == nil { sourceSize = await PhotoAdjustmentRenderer.sourceSize(at: input) }
        }
        .onChange(of: draft.settings) { _, _ in showBefore = false }
        .onChange(of: composition) { _, value in if value { draft.cancelPicker() } }
        .onChange(of: showBefore) { _, value in if value { draft.cancelPicker() } }
        .onDisappear { draft.cancelPicker(); model.photoPreview.clear() }
    }

    private func sectionTitle(_ title: String) -> some View {
        HStack(spacing: 8) {
            Text(title).font(.caption.weight(.medium)).foregroundStyle(.secondary)
            Rectangle().fill(.separator).frame(height: 1)
        }
        .padding(.top, 4)
    }

    private func adjustmentRows(_ controls: [PhotoAdjustments.Control]) -> some View {
        VStack(spacing: 1) {
            ForEach(controls, id: \.self) { control in
                ParameterSlider(title: control.title(chinese: chinese), value: Binding(
                    get: { draft.settings[control] },
                    set: { draft.settings[control] = $0; showBefore = false }
                ), range: draft.settings.allowedRange(for: control), continuous: true,
                   resetValue: control.neutralValue, resetLabel: chinese ? "重置" : "Reset")
                    .accessibilityIdentifier("finish." + control.rawValue)
            }
        }
    }

    private struct PreviewRequest: Equatable {
        let input: URL
        let settings: PhotoAdjustments
        let showBefore: Bool
        let warnings: PhotoClippingWarnings
        let picking: Bool
    }
}


private struct PhotoPreviewError: View {
    @ObservedObject var preview: PhotoAdjustmentPreview
    var body: some View {
        if let message = preview.errorMessage {
            Text(message).font(.caption).foregroundStyle(.red)
        }
    }
}

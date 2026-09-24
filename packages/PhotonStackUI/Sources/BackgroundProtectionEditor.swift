import AppKit
import PhotonStackAppCore
import SwiftUI

struct BackgroundProtectionEditor: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let report: DeepSkyReport
    @Environment(\.dismiss) private var dismiss
    @State private var ellipses: [BackgroundProtectionEllipse] = []
    @State private var selected: Int?
    @State private var drawing = false
    @State private var draft: BackgroundProtectionEllipse?
    @State private var moveStart: BackgroundProtectionEllipse?
    @State private var image: CGImage?
    @State private var failure: String?
    @State private var original = ""
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var crop: CGRect {
        guard let c = report.crop else { return .zero }
        return CGRect(x: c.x, y: c.y, width: c.width, height: c.height)
    }
    private var canApply: Bool {
        image != nil && failure == nil && ellipses.allSatisfy(\.valid) && model.canRefinishDeepSkyMaster
            && model.deepSkySettings.lastReport == report
            && (model.deepSkySettings.values["background.exclusions"] ?? "") == original
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(chinese ? "背景保护区" : "Background protection").font(.title2)
            Text(chinese ? "覆盖星云、星系及外围光晕，留出足够天空估计背景。这里显示上次成片；保存范围后，点击“更新背景与降噪”生成新结果。"
                 : "Cover the target and faint outskirts, leaving enough sky for background estimation. This is the previous result. Save regions, then reprocess the master.")
                .font(.callout).foregroundStyle(.secondary)
            HStack(alignment: .top, spacing: 16) {
                canvas.frame(minWidth: 600, minHeight: 460)
                VStack(alignment: .leading, spacing: 10) {
                    Toggle(chinese ? "拖动绘制新椭圆" : "Drag to draw an ellipse", isOn: $drawing)
                        .toggleStyle(.button).accessibilityIdentifier("background-protection.draw")
                    Text(chinese ? "未开启绘制时，拖动所选椭圆可移动位置。" : "When drawing is off, drag the selected ellipse to move it.")
                        .font(.caption).foregroundStyle(.secondary)
                    List(selection: $selected) {
                        ForEach(ellipses.indices, id: \.self) { i in
                            Text(chinese ? "保护区 \(i + 1)" : "Region \(i + 1)").tag(i)
                        }
                    }.frame(height: 120)
                    if let i = selected, ellipses.indices.contains(i) {
                        valueField(chinese ? "中心 X" : "Center X", index: i, key: \.x)
                        valueField(chinese ? "中心 Y" : "Center Y", index: i, key: \.y)
                        valueField(chinese ? "半轴 A" : "Semiaxis A", index: i, key: \.a)
                        valueField(chinese ? "半轴 B" : "Semiaxis B", index: i, key: \.b)
                        valueField(chinese ? "角度（°）" : "Angle (°)", index: i, key: \.angle)
                        Slider(value: Binding(get: { ellipses[i].angle }, set: { ellipses[i].angle = $0 }), in: -180...180)
                            .accessibilityLabel(chinese ? "旋转保护区" : "Rotate region")
                        Button(chinese ? "移除所选区域" : "Remove selected region") {
                            ellipses.remove(at: i); selected = ellipses.isEmpty ? nil : min(i, ellipses.count - 1)
                        }.accessibilityIdentifier("background-protection.remove")
                    }
                    Spacer()
                }.frame(width: 250).disabled(image == nil || failure != nil)
            }
            if let failure { Text(failure).foregroundStyle(.red).font(.caption) }
            if !ellipses.allSatisfy(\.valid) {
                Text(chinese ? "坐标必须是有效数值，两个半轴必须大于 0。" : "Use finite coordinates and positive semiaxes.").foregroundStyle(.red).font(.caption)
            }
            HStack {
                Text(chinese ? "\(ellipses.count) 个保护区 · 原始参考坐标" : "\(ellipses.count) regions · reference coordinates").foregroundStyle(.secondary)
                Spacer()
                Button(chinese ? "取消" : "Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button(chinese ? "保存保护区" : "Save regions") {
                    guard canApply else { return }
                    let text = BackgroundProtectionEllipse.parse(original) == ellipses ? original : BackgroundProtectionEllipse.recipe(ellipses)
                    model.setDeepSkyValue(text, for: "background.exclusions")
                    dismiss()
                }.buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction)
                    .disabled(!canApply).accessibilityIdentifier("background-protection.save")
            }
        }.padding(18).frame(width: 920, height: 640)
            .task { await load() }
    }

    private func valueField(_ title: String, index: Int, key: WritableKeyPath<BackgroundProtectionEllipse, Double>) -> some View {
        HStack {
            Text(title).frame(width: 88, alignment: .leading)
            TextField(title, value: Binding(get: { ellipses[index][keyPath: key] }, set: { ellipses[index][keyPath: key] = $0 }), format: .number)
                .textFieldStyle(.roundedBorder)
        }
    }

    private var canvas: some View {
        GeometryReader { proxy in
            let geometry = BackgroundProtectionGeometry(crop: crop, size: proxy.size)
            ZStack(alignment: .topLeading) {
                Color.black
                if let image {
                    Image(decorative: image, scale: 1).resizable()
                        .frame(width: geometry.imageRect.width, height: geometry.imageRect.height)
                        .position(x: geometry.imageRect.midX, y: geometry.imageRect.midY)
                    ForEach(ellipses.indices, id: \.self) { i in
                        ellipsePath(ellipses[i], geometry: geometry)
                            .stroke(i == selected ? Color.orange : Color.cyan, lineWidth: i == selected ? 2.5 : 1.5)
                    }
                    if let draft { ellipsePath(draft, geometry: geometry).stroke(Color.yellow, style: StrokeStyle(lineWidth: 2, dash: [5])) }
                } else if failure == nil { ProgressView().position(x: proxy.size.width / 2, y: proxy.size.height / 2) }
            }.contentShape(Rectangle()).clipped()
                .accessibilityLabel(chinese ? "在成片上编辑背景保护椭圆" : "Edit background ellipses on the result")
                .accessibilityIdentifier("background-protection.canvas")
                .gesture(DragGesture(minimumDistance: 3).onChanged { value in
                    guard image != nil, failure == nil else { return }
                    if drawing { draft = geometry.drawn(from: value.startLocation, to: value.location) }
                    else if let i = selected, ellipses.indices.contains(i), geometry.imageRect.contains(value.startLocation) {
                        if moveStart == nil, ellipses[i].contains(geometry.masterPoint(value.startLocation)) { moveStart = ellipses[i] }
                        if let start = moveStart {
                            ellipses[i].x = start.x + value.translation.width / geometry.scale
                            ellipses[i].y = start.y + value.translation.height / geometry.scale
                        }
                    }
                }.onEnded { _ in
                    if let draft { ellipses.append(draft); selected = ellipses.count - 1; drawing = false }
                    draft = nil; moveStart = nil
                })
        }
    }

    private func ellipsePath(_ e: BackgroundProtectionEllipse, geometry: BackgroundProtectionGeometry) -> Path {
        guard e.valid else { return Path() }
        let center = geometry.canvasPoint(CGPoint(x: e.x, y: e.y))
        let rect = CGRect(x: -e.a * geometry.scale, y: -e.b * geometry.scale,
                          width: 2 * e.a * geometry.scale, height: 2 * e.b * geometry.scale)
        return Path(ellipseIn: rect).applying(CGAffineTransform(rotationAngle: e.angle * .pi / 180)
            .concatenating(CGAffineTransform(translationX: center.x, y: center.y)))
    }

    private func load() async {
        original = model.deepSkySettings.values["background.exclusions"] ?? ""
        guard let parsed = BackgroundProtectionEllipse.parse(original) else {
            failure = chinese ? "现有保护区格式无效，请先修正坐标文本。" : "Existing regions are invalid. Correct the coordinate text first."; return
        }
        ellipses = parsed; selected = parsed.isEmpty ? nil : 0; drawing = parsed.isEmpty
        guard crop.width > 0, crop.height > 0, crop.minX >= 0, crop.minY >= 0 else {
            failure = chinese ? "这份结果缺少有效裁边坐标，请重新生成成片后编辑。" : "Missing crop coordinates. Generate a new result before editing."; return
        }
        do {
            let loader = NativePreviewImageLoader()
            guard let decoded = try await loader.load(URL(fileURLWithPath: report.developed)),
                  Double(decoded.width) == crop.width, Double(decoded.height) == crop.height else {
                throw CocoaError(.fileReadCorruptFile)
            }
            try Task.checkCancellation()
            image = decoded
        } catch is CancellationError { }
        catch { failure = chinese ? "成片无法读取或尺寸与裁边记录不符，已停止编辑以免坐标错位。" : "The result is unreadable or its dimensions do not match the crop. Editing is disabled." }
    }
}

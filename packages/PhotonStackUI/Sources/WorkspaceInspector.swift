import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct InspectorPanel: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var page: WorkspaceInspectorPage
    var isFinishing: Bool
    @State private var expandedGroups: Set<InspectorToolGroup> = Set(InspectorToolGroup.defaultExpanded)
    private let groupAnimation = Animation.interactiveSpring(response: 0.28, dampingFraction: 0.9, blendDuration: 0.08)

    var body: some View {
        VStack(spacing: 0) {
            Picker(model.language == .simplifiedChinese ? "检查器" : "Inspector", selection: $page) {
                ForEach(WorkspaceInspectorPage.allCases, id: \.self) { page in
                    Text(page.title(chinese: model.language == .simplifiedChinese)).tag(page)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .accessibilityIdentifier("workspace.inspector.pages")
            .padding(12)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    if page == .assets {
                        InspectorToolGroupView(
                            model: model,
                            group: .asset,
                            summary: summary(for: .asset),
                            isExpanded: binding(for: .asset)
                        ) {
                            SelectedAssetSection(model: model)
                        }

                        InspectorToolGroupView(
                            model: model,
                            group: .photoInfo,
                            summary: summary(for: .photoInfo),
                            isExpanded: binding(for: .photoInfo)
                        ) {
                            PhotoInfoSection(model: model)
                        }

                    }

                    if page == .processing {
                        if isFinishing {
                            Text(model.language == .simplifiedChinese ? "后期调整" : "Finish your image")
                                .font(.headline)
                            AdjustmentsInspectorSection(model: model, parameters: processingParametersBinding)
                        } else {
                            InspectorToolGroupView(
                                model: model,
                                group: .workflow,
                                summary: summary(for: .workflow),
                                isExpanded: binding(for: .workflow)
                            ) {
                                DeepSkyWorkflowOverview(model: model)
                            }
                            Button(model.language == .simplifiedChinese ? "直接调整当前图像…" : "Edit current image…") {
                                NotificationCenter.default.post(name: .photonStackOpenFinishing, object: nil)
                            }
                            .disabled(!model.canProcessCurrentInput || model.hasActiveProcessing)
                            Text(model.language == .simplifiedChinese ? "适合单张照片或已经堆好的成片。不会自动堆栈。" : "For a single photo or an existing master. This does not run stacking.")
                                .font(.caption).foregroundStyle(.secondary)
                        }

                    }

                    if page == .tools {
                        DisclosureGroup(model.localized(.registrationSection)) {
                            RegistrationSection(model: model)
                        }
                        DisclosureGroup(model.language == .simplifiedChinese ? "拼接" : "Mosaic") {
                            MosaicSection(model: model, parameters: processingParametersBinding)
                        }

                        InspectorToolGroupView(
                            model: model,
                            group: .workspace,
                            summary: summary(for: .workspace),
                            isExpanded: binding(for: .workspace)
                        ) {
                            ProcessingWorkspaceSection(model: model)
                        }

                        InspectorToolGroupView(
                            model: model,
                            group: .batch,
                            summary: summary(for: .batch),
                            isExpanded: binding(for: .batch)
                        ) {
                            BatchQueueSection(model: model)
                        }

                    }

                    if page == .project {
                        WorkspaceStorageView(model: model)
                        Divider()
                        InspectorToolGroupView(
                            model: model,
                            group: .history,
                            summary: summary(for: .history),
                            isExpanded: binding(for: .history)
                        ) {
                            EditGraphSection(model: model)
                            JobsSection(jobs: model.jobs, model: model)
                        }

                        InspectorToolGroupView(
                            model: model,
                            group: .project,
                            summary: summary(for: .project),
                            isExpanded: binding(for: .project)
                        ) {
                            LanguageSection(model: model)
                            ProjectSection(model: model)
                        }
                    }
                }
                .padding(16)
            }

            Divider()

            StatusConsole(model: model)
        }
    }

    private func binding(for group: InspectorToolGroup) -> Binding<Bool> {
        Binding(
            get: { expandedGroups.contains(group) },
            set: { isExpanded in
                withAnimation(groupAnimation) {
                    if isExpanded {
                        expandedGroups.insert(group)
                    } else {
                        expandedGroups.remove(group)
                    }
                }
            }
        )
    }

    private var processingParametersBinding: Binding<ProcessingParameters> {
        Binding(
            get: { model.parameters },
            set: { model.setProcessingParameters($0) }
        )
    }

    private func summary(for group: InspectorToolGroup) -> String? {
        switch group {
        case .project:
            return model.project.name
        case .asset:
            return model.selectedAsset?.displayName ?? model.localized(.noAssetSelected)
        case .photoInfo:
            guard let metadata = model.selectedAsset?.metadata else {
                return model.localized(.metadataUnavailable)
            }
            return [dimensions(metadata), cameraName(metadata), exposure(metadata)]
                .compactMap(\.self)
                .joined(separator: " · ")
        case .workspace:
            return "\(model.project.layers.count) \(model.localized(.layersLabel)) · \(model.project.artifacts.count) \(model.localized(.artifactsLabel))"
        case .workflow:
            return "\(model.roleName(.light)) \(model.lightAssets.count) · \(model.roleName(.dark)) \(model.darkAssets.count) · \(model.roleName(.flat)) \(model.flatAssets.count)"
        case .batch:
            return "\(model.batchQueue.count) \(model.localized(.tasksSection))"
        case .history:
            return "\(model.project.editGraph.operations.count) \(model.localized(.editGraphSection)) · \(model.jobs.count) \(model.localized(.tasksSection))"
        }
    }

    private func dimensions(_ metadata: AssetMetadata) -> String? {
        guard let width = metadata.width, let height = metadata.height else {
            return nil
        }
        return "\(width) x \(height)"
    }

    private func cameraName(_ metadata: AssetMetadata) -> String? {
        let name = [metadata.cameraMake, metadata.cameraModel]
            .compactMap { value in
                let trimmed = value?.trimmingCharacters(in: .whitespacesAndNewlines)
                return trimmed?.isEmpty == false ? trimmed : nil
            }
            .joined(separator: " ")
        return name.isEmpty ? nil : name
    }

    private func exposure(_ metadata: AssetMetadata) -> String? {
        guard let exposure = metadata.exposureTimeSeconds, exposure.isFinite, exposure > 0 else {
            return nil
        }
        if exposure > 0, exposure < 1 {
            let denominator = (1.0 / exposure).rounded()
            if denominator.isFinite, denominator <= Double(Int.max) {
                return "1/\(Int(denominator)) s"
            }
        }
        return exposure < 0.01 ? String(format: "%.3g s", exposure) : String(format: "%.2f s", exposure)
    }
}

private enum InspectorToolGroup: String, CaseIterable, Hashable {
    case project
    case asset
    case photoInfo
    case workspace
    case workflow
    case batch
    case history

    static var defaultExpanded: [InspectorToolGroup] {
        [.asset, .workflow, .history]
    }

    var titleKey: LocalizedTextKey {
        switch self {
        case .project:
            return .inspectorProjectGroup
        case .asset:
            return .inspectorAssetGroup
        case .photoInfo:
            return .inspectorPhotoInfoGroup
        case .workspace:
            return .inspectorWorkspaceGroup
        case .workflow:
            return .inspectorWorkflowGroup
        case .batch:
            return .inspectorBatchGroup
        case .history:
            return .inspectorHistoryGroup
        }
    }

    var iconName: String {
        switch self {
        case .project:
            return "folder"
        case .asset:
            return "photo.on.rectangle"
        case .photoInfo:
            return "camera.metering.matrix"
        case .workspace:
            return "rectangle.3.group"
        case .workflow:
            return "point.3.connected.trianglepath.dotted"
        case .batch:
            return "tray.full"
        case .history:
            return "clock.arrow.circlepath"
        }
    }

    var introKey: LocalizedTextKey {
        switch self {
        case .project:
            return .inspectorProjectGroupIntro
        case .asset:
            return .inspectorAssetGroupIntro
        case .photoInfo:
            return .inspectorPhotoInfoGroupIntro
        case .workspace:
            return .inspectorWorkspaceGroupIntro
        case .workflow:
            return .inspectorWorkflowGroupIntro
        case .batch:
            return .inspectorBatchGroupIntro
        case .history:
            return .inspectorHistoryGroupIntro
        }
    }
}

private struct InspectorToolGroupView<Content: View>: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let group: InspectorToolGroup
    var summary: String? = nil
    @Binding var isExpanded: Bool
    @ViewBuilder let content: () -> Content
    @State private var showIntro = false
    @State private var hoverIntroTask: Task<Void, Never>?
    private let groupAnimation = Animation.interactiveSpring(response: 0.28, dampingFraction: 0.9, blendDuration: 0.08)

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button {
                isExpanded.toggle()
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: group.iconName)
                        .foregroundStyle(Color.accentColor)
                        .frame(width: 18)
                        .transaction { transaction in
                            transaction.animation = nil
                        }

                    VStack(alignment: .leading, spacing: 2) {
                        Text(model.localized(group.titleKey))
                            .font(.subheadline)
                            .fontWeight(.semibold)
                            .transaction { transaction in
                                transaction.animation = nil
                            }

                        if let summary, summary.isEmpty == false {
                            Text(summary)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .transaction { transaction in
                                    transaction.animation = nil
                                }
                        }
                    }

                    Spacer()

                    Image(systemName: "chevron.right")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .frame(width: 14)
                        .rotationEffect(.degrees(isExpanded ? 90 : 0))
                        .animation(groupAnimation, value: isExpanded)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(InspectorHeaderButtonStyle())
            .onHover { isHovering in
                if isHovering {
                    scheduleIntro()
                } else {
                    cancelIntro()
                }
            }
            .popover(isPresented: $showIntro, arrowEdge: .trailing) {
                Text(model.localized(group.introKey))
                    .font(.callout)
                    .lineSpacing(3)
                    .foregroundStyle(.primary)
                    .frame(width: 280, alignment: .leading)
                    .padding(14)
                    .presentationCompactAdaptation(.popover)
            }

            CollapsibleInspectorContent(isExpanded: isExpanded) {
                content()
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 8))
        .overlay {
            RoundedRectangle(cornerRadius: 8)
                .stroke(.secondary.opacity(0.16), lineWidth: 1)
        }
        .onDisappear {
            cancelIntro()
        }
    }

    private func scheduleIntro() {
        hoverIntroTask?.cancel()
        showIntro = false
        hoverIntroTask = Task {
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            guard Task.isCancelled == false else {
                return
            }
            await MainActor.run {
                withAnimation(.easeOut(duration: 0.16)) {
                    showIntro = true
                }
            }
        }
    }

    private func cancelIntro() {
        hoverIntroTask?.cancel()
        hoverIntroTask = nil
        showIntro = false
    }
}

private struct InspectorHeaderButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
    }
}

private struct CollapsibleInspectorContent<Content: View>: View {
    let isExpanded: Bool
    @ViewBuilder let content: () -> Content
    @State private var measuredHeight: CGFloat = 0
    private let groupAnimation = Animation.interactiveSpring(response: 0.28, dampingFraction: 0.9, blendDuration: 0.08)

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            content()
        }
        .padding(.top, 12)
        .fixedSize(horizontal: false, vertical: true)
        .background {
            GeometryReader { proxy in
                Color.clear
                    .preference(key: InspectorContentHeightKey.self, value: proxy.size.height)
            }
        }
        .onPreferenceChange(InspectorContentHeightKey.self) { height in
            guard abs(measuredHeight - height) > 0.5 else {
                return
            }
            measuredHeight = height
        }
        .frame(height: isExpanded ? measuredHeight : 0, alignment: .top)
        .opacity(isExpanded ? 1 : 0)
        .clipped()
        .allowsHitTesting(isExpanded)
        .accessibilityHidden(isExpanded == false)
        .animation(groupAnimation, value: isExpanded)
    }
}

private struct InspectorContentHeightKey: PreferenceKey {
    static let defaultValue: CGFloat = 0

    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = max(value, nextValue())
    }
}

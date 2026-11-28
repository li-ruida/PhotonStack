import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct AssetAvailability: ViewModifier {
    let url: URL
    @Binding var missing: Bool
    @Environment(\.scenePhase) private var phase

    private struct Request: Equatable {
        let url: URL
        let phase: ScenePhase
    }

    func body(content: Content) -> some View {
        content.task(id: Request(url: url, phase: phase)) {
            let path = url.path
            let result = await Task.detached(priority: .utility) {
                var directory: ObjCBool = false
                return !FileManager.default.fileExists(atPath: path, isDirectory: &directory) || directory.boolValue
            }.value
            if !Task.isCancelled { missing = result }
        }
    }
}

struct AssetSidebar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let onImportFiles: () -> Void
    let onImportFolder: () -> Void
    @State private var hoveredAssetID: PhotonStackAsset.ID?

    @State private var expandedPanels: Set<PhotonStackAsset.ID> = []
    @State private var selectedFrame: SidebarSelection?

    var body: some View {
        let showingResult = model.isShowingLatestDeepSkyResult || model.isShowingMosaicResult || model.isShowingMosaicPreview
        let selection = sidebarSelection(showingResult: showingResult)
        VStack(spacing: 0) {
            if model.latestMosaicOperation != nil {
                Button { model.showLatestMosaicResult() } label: {
                    HStack {
                        Label(model.language == .simplifiedChinese ? "拼接成片" : "Mosaic result", systemImage: "rectangle.3.group")
                        Spacer()
                        if model.isShowingMosaicResult { Image(systemName: "checkmark") }
                    }
                    .padding(8)
                    .background(model.isShowingMosaicResult ? Color.accentColor.opacity(0.16) : .clear,
                                in: RoundedRectangle(cornerRadius: 6))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .padding(.horizontal, 8)
                .disabled(!model.canModifyProcessingConfiguration)
                .accessibilityAddTraits(model.isShowingMosaicResult ? .isSelected : [])
                .accessibilityIdentifier("sidebar.mosaic-result")
                Divider()
            }
            if model.latestDeepSkyResultURL != nil {
                Button {
                    model.showLatestDeepSkyResult()
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: "photo.stack")
                        Text(model.language == .simplifiedChinese ? "堆栈成片" : "Stack result")
                        Spacer()
                        if model.isShowingLatestDeepSkyResult {
                            Image(systemName: "checkmark").accessibilityHidden(true)
                        }
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 7)
                    .background(model.isShowingLatestDeepSkyResult ? Color.accentColor.opacity(0.16) : .clear,
                                in: RoundedRectangle(cornerRadius: 6))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(!model.canModifyProcessingConfiguration)
                .accessibilityAddTraits(model.isShowingLatestDeepSkyResult ? .isSelected : [])
                .accessibilityIdentifier("sidebar.stack-result")
                .padding(.horizontal, 8)
                .padding(.vertical, 6)
                Divider()
            }
            if model.project.assets.isEmpty {
                Spacer(minLength: 20)

                VStack(spacing: 12) {
                    Image(systemName: "photo.on.rectangle.angled")
                        .font(.system(size: 38, weight: .regular))
                        .foregroundStyle(.secondary)

                    Text(model.localized(.emptyAssetsTitle))
                        .font(.headline)

                    Menu {
                        Button(action: onImportFiles) {
                            Label(model.localized(.importFilesAction), systemImage: "photo.on.rectangle.angled")
                        }
                        Button(action: onImportFolder) {
                            Label(model.localized(.importFolderAction), systemImage: "folder.badge.plus")
                        }
                    } label: {
                        Label(model.localized(.importAction), systemImage: "plus")
                            .frame(maxWidth: 180)
                    }
                    .buttonStyle(.borderedProminent)
                    .help(model.localized(.emptyAssetsMessage))
                    .disabled(model.isProcessing)
                }
                .padding(20)

                Spacer(minLength: 20)
            } else {
                List(selection: Binding(get: { selection }, set: select)) {
                    ForEach(assetGroups) { group in
                        Section {
                            ForEach(group.assets) { asset in
                                let folder = group.role == .mosaic ? asset.mosaicFolder : nil
                                let expanded = expandedPanels.contains(asset.id)
                                let row = SidebarSelection.asset(asset.id)
                                HStack(spacing: 4) {
                                    if folder != nil {
                                        Button { togglePanel(asset.id) } label: {
                                            Image(systemName: expanded ? "chevron.down" : "chevron.right")
                                                .font(.caption.bold())
                                                .frame(width: 18, height: 24)
                                                .contentShape(Rectangle())
                                        }
                                        .buttonStyle(.plain)
                                        .accessibilityLabel(disclosureLabel(expanded: expanded) + " " + asset.displayName)
                                    }
                                    SidebarAssetRow(model: model, asset: asset,
                                                    isSelected: selection == row,
                                                    showsRemoveButton: hoveredAssetID == asset.id || selection == row)
                                }
                                .tag(row)
                                .contentShape(Rectangle())
                                .onHover { hoveredAssetID = $0 ? asset.id : nil }

                                if expanded, let folder {
                                    ForEach(folder.frames, id: \.self) { frame in
                                        Label(frame.lastPathComponent, systemImage: "photo")
                                            .font(.callout)
                                            .lineLimit(1)
                                            .truncationMode(.middle)
                                            .padding(.leading, 22)
                                            .help(frame.path)
                                            .tag(SidebarSelection.frame(asset.id, frame))
                                    }
                                }
                            }
                        } header: {
                            HStack(spacing: 6) {
                                Text(model.roleName(group.role))
                                Spacer()
                                Text("\(group.assets.count)")
                                    .font(.caption2.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }
                .listStyle(.sidebar)
                .contextMenu(forSelectionType: SidebarSelection.self) { rows in
                    if case let .asset(id) = rows.first, let asset = model.project.assets.first(where: { $0.id == id }) {
                        Button { presentRelinkPanel(for: asset, model: model) } label: {
                            Label(model.localized(.relinkAssetAction), systemImage: "link")
                        }
                        .disabled(model.isProcessing)
                        Divider()
                        Button(role: .destructive) { model.requestAssetRemoval(asset) } label: {
                            Label(model.localized(.removeAssetAction), systemImage: "trash")
                        }
                        .disabled(model.isProcessing)
                    }
                } primaryAction: { rows in
                    if case let .asset(id) = rows.first, model.usesMosaicWorkflow,
                       model.mosaicAssets.contains(where: { $0.id == id && $0.mosaicFolder != nil }) {
                        togglePanel(id)
                    }
                }
                .onDeleteCommand {
                    if case let .asset(id) = selection, let asset = model.project.assets.first(where: { $0.id == id }),
                       !model.isProcessing {
                        model.requestAssetRemoval(asset)
                    }
                }
            }

            Divider()

            BuildIdentityBadge()
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
        }
        .onChange(of: model.project.id) {
            expandedPanels.removeAll()
            selectedFrame = nil
        }
        .navigationTitle(model.project.name)
        .navigationSplitViewColumnWidth(min: 220, ideal: 280, max: 360)
    }

    private var assetGroups: [SidebarAssetGroup] {
        let panels = model.usesMosaicWorkflow ? model.mosaicAssets : []
        let panelIDs = Set(panels.map(\.id))
        let groupedSources = Set(panels.flatMap { $0.mosaicFolder?.frames ?? [] }.map(PhotonStackProject.assetIdentityPath(for:)))
        let visible = model.project.assets.filter {
            panelIDs.contains($0.id) || $0.role != .light || !groupedSources.contains(PhotonStackProject.assetIdentityPath(for: $0.originalURL))
        }
        return CalibrationFrameRole.allCases.compactMap { role in
            let assets = visible.filter { (panelIDs.contains($0.id) ? .mosaic : $0.role) == role }
            return assets.isEmpty ? nil : SidebarAssetGroup(role: role, assets: assets)
        }
    }

    private func sidebarSelection(showingResult: Bool) -> SidebarSelection? {
        guard !showingResult, let id = model.selectedAssetID else { return nil }
        if model.usesMosaicWorkflow, model.canvasMode == .sourcePreview,
           let url = model.previewURL, let asset = model.selectedAsset,
           url != asset.originalURL || selectedFrame == .frame(id, url),
           asset.mosaicFolder?.frames.contains(url) == true {
            return expandedPanels.contains(id) ? .frame(id, url) : nil
        }
        return .asset(id)
    }

    private func select(_ row: SidebarSelection?) {
        guard model.canModifyProcessingConfiguration, let row else { return }
        switch row {
        case let .asset(id):
            guard let asset = model.project.assets.first(where: { $0.id == id }) else { return }
            selectedFrame = nil
            model.select(asset)
        case let .frame(id, url):
            if model.previewMosaicFrame(url, in: id) { selectedFrame = row }
        }
    }

    private func togglePanel(_ id: PhotonStackAsset.ID) {
        if !expandedPanels.insert(id).inserted { expandedPanels.remove(id) }
    }

    private func disclosureLabel(expanded: Bool) -> String {
        if model.language == .simplifiedChinese { return expanded ? "收起原片" : "展开原片" }
        return expanded ? "Collapse frames" : "Expand frames"
    }
}

private enum SidebarSelection: Hashable {
    case asset(PhotonStackAsset.ID)
    case frame(PhotonStackAsset.ID, URL)
}

private struct SidebarAssetGroup: Identifiable {
    var role: CalibrationFrameRole
    var assets: [PhotonStackAsset]

    var id: CalibrationFrameRole {
        role
    }
}

private struct BuildIdentityBadge: View {
    private let identity = PhotonStackBuildIdentity.current

    var body: some View {
        Text("v\(identity.version)")
        .font(.caption2)
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .leading)
        .help(identity.helpText)
        .accessibilityLabel("PhotonStack \(identity.displayText)")
    }
}

private struct SidebarAssetRow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let asset: PhotonStackAsset
    let isSelected: Bool
    let showsRemoveButton: Bool

    @State private var missing = false

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: iconName)
                .foregroundStyle(
                    missing
                        ? Color.orange
                        : (isSelected ? Color.accentColor : Color.secondary)
                )
                .frame(width: 18)

            VStack(alignment: .leading, spacing: 4) {
                Text(asset.displayName)
                    .lineLimit(1)
                Text(subtitle)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            Spacer(minLength: 6)

            Button {
                model.requestAssetRemoval(asset)
            } label: {
                Label(model.localized(.removeAssetAction), systemImage: "trash")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.removeAssetHelp))
            .disabled(model.isProcessing)
            .opacity(showsRemoveButton ? 1 : 0)
            .accessibilityHidden(showsRemoveButton == false)
        }
        .padding(.vertical, 4)
        .modifier(AssetAvailability(url: asset.originalURL, missing: $missing))
    }

    private var subtitle: String {
        if missing {
            return model.localized(.missingAssetStatus)
        }
        if let group = asset.mosaicFolder {
            return model.language == .simplifiedChinese ? "\(group.frames.count) 帧 · \(asset.kind.rawValue.uppercased())"
                : "\(group.frames.count) frames · \(asset.kind.rawValue.uppercased())"
        }
        var parts = [asset.kind.rawValue.uppercased()]
        if let width = asset.metadata?.width, let height = asset.metadata?.height {
            parts.append("\(width) x \(height)")
        } else {
            let folder = asset.originalURL.deletingLastPathComponent().lastPathComponent
            if folder.isEmpty == false {
                parts.append(folder)
            }
        }
        return parts.joined(separator: " · ")
    }

    private var iconName: String {
        if missing {
            return "exclamationmark.triangle.fill"
        }
        switch asset.kind {
        case .raw:
            return "camera"
        case .fits:
            return "sparkles"
        case .tiff, .jpeg, .heif, .png:
            return "photo"
        case .unknown:
            return "doc"
        }
    }
}

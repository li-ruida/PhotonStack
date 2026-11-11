import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct AssetSidebar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let onImportFiles: () -> Void
    let onImportFolder: () -> Void
    @State private var hoveredAssetID: PhotonStackAsset.ID?

    var body: some View {
        let showingResult = model.isShowingLatestDeepSkyResult
        VStack(spacing: 0) {
            if model.latestDeepSkyResultURL != nil {
                Button {
                    model.showLatestDeepSkyResult()
                } label: {
                    HStack(spacing: 8) {
                        Image(systemName: "photo.stack")
                        Text(model.language == .simplifiedChinese ? "堆栈成片" : "Stack result")
                        Spacer()
                        if showingResult {
                            Image(systemName: "checkmark").accessibilityHidden(true)
                        }
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 7)
                    .background(showingResult ? Color.accentColor.opacity(0.16) : .clear,
                                in: RoundedRectangle(cornerRadius: 6))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(!model.canModifyProcessingConfiguration)
                .accessibilityAddTraits(showingResult ? .isSelected : [])
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

                    VStack(spacing: 4) {
                        Text(model.localized(.emptyAssetsTitle))
                            .font(.headline)
                        Text(model.localized(.emptyAssetsMessage))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }

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
                    .disabled(model.isProcessing)
                }
                .padding(20)

                Spacer(minLength: 20)
            } else {
                List(selection: assetSelection(showingResult: showingResult)) {
                    ForEach(assetGroups) { group in
                        Section {
                            ForEach(group.assets) { asset in
                                SidebarAssetRow(
                                    model: model,
                                    asset: asset,
                                    isSelected: !showingResult && asset.id == model.selectedAsset?.id,
                                    showsRemoveButton: hoveredAssetID == asset.id || (!showingResult && asset.id == model.selectedAssetID)
                                )
                                .tag(asset.id)
                                .contentShape(Rectangle())
                                .onHover { isHovering in
                                    hoveredAssetID = isHovering ? asset.id : nil
                                }
                                .contextMenu {
                                    Button {
                                        presentRelinkPanel(for: asset, model: model)
                                    } label: {
                                        Label(model.localized(.relinkAssetAction), systemImage: "link")
                                    }
                                    .disabled(model.isProcessing)

                                    Divider()

                                    Button(role: .destructive) {
                                        model.requestAssetRemoval(asset)
                                    } label: {
                                        Label(model.localized(.removeAssetAction), systemImage: "trash")
                                    }
                                    .disabled(model.isProcessing)
                                }
                            }
                            .onDelete { offsets in
                                model.requestAssetRemoval(
                                    offsets.sorted().map { group.assets[$0] }
                                )
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
                .onDeleteCommand {
                    if !showingResult, let asset = model.selectedAsset, model.isProcessing == false {
                        model.requestAssetRemoval(asset)
                    }
                }
            }

            Divider()

            BuildIdentityBadge()
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
        }
        .navigationTitle(model.project.name)
        .navigationSplitViewColumnWidth(min: 220, ideal: 280, max: 360)
    }

    private var assetGroups: [SidebarAssetGroup] {
        CalibrationFrameRole.allCases.compactMap { role in
            let assets = model.project.assets.filter { $0.role == role }
            return assets.isEmpty ? nil : SidebarAssetGroup(role: role, assets: assets)
        }
    }

    private func assetSelection(showingResult: Bool) -> Binding<PhotonStackAsset.ID?> {
        Binding(
            get: { showingResult ? nil : model.selectedAssetID },
            set: { assetID in
                guard model.isProcessing == false,
                      let assetID,
                      let asset = model.project.assets.first(where: { $0.id == assetID })
                else {
                    return
                }
                model.select(asset)
            }
        )
    }
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
        HStack(spacing: 6) {
            Image(systemName: "number")
                .imageScale(.small)

            Text(identity.displayText)
                .lineLimit(1)
                .truncationMode(.middle)
        }
        .font(.caption2.monospaced())
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

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: iconName)
                .foregroundStyle(
                    model.isAssetMissing(asset)
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
    }

    private var subtitle: String {
        if model.isAssetMissing(asset) {
            return model.localized(.missingAssetStatus)
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
        if model.isAssetMissing(asset) {
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

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
        VStack(spacing: 0) {
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
                List(selection: assetSelection) {
                    ForEach(assetGroups) { group in
                        Section {
                            ForEach(group.assets) { asset in
                                SidebarAssetRow(
                                    model: model,
                                    asset: asset,
                                    isSelected: asset.id == model.selectedAsset?.id,
                                    showsRemoveButton: hoveredAssetID == asset.id || asset.id == model.selectedAssetID
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
                    if let asset = model.selectedAsset, model.isProcessing == false {
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

    private var assetSelection: Binding<PhotonStackAsset.ID?> {
        Binding(
            get: { model.selectedAssetID },
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

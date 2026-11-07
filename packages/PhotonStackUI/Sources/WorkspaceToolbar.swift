import SwiftUI

struct WorkspaceToolbar: ToolbarContent {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let onNewProject: () -> Void
    let onImportFiles: () -> Void
    let onImportFolder: () -> Void
    let onOpenProject: () -> Void
    let onSaveProject: () -> Void
    let onOpenWorkflows: () -> Void

    var body: some ToolbarContent {
        ToolbarItemGroup(placement: .automatic) {
            Button(action: onNewProject) {
                Label(model.localized(.newProject), systemImage: "doc.badge.plus")
                    .labelStyle(.titleAndIcon)
            }
            .help(model.localized(.newProject))
            .keyboardShortcut("n", modifiers: [.command])
            .accessibilityIdentifier("workspace.newProject")
            .disabled(model.hasActiveProcessing)

            Menu {
                Button(action: onImportFiles) {
                    Label(model.localized(.importFilesAction), systemImage: "photo.on.rectangle.angled")
                }
                .keyboardShortcut("i", modifiers: [.command])

                Button(action: onImportFolder) {
                    Label(model.localized(.importFolderAction), systemImage: "folder.badge.plus")
                }
                .keyboardShortcut("i", modifiers: [.command, .shift])
            } label: {
                Label(model.localized(.importAction), systemImage: "plus")
                    .labelStyle(.titleAndIcon)
            }
            .help(model.localized(.importAction))
            .accessibilityIdentifier("workspace.import")
            .disabled(model.hasActiveProcessing)

            Button(action: onOpenWorkflows) {
                Label(model.localized(.workflowsAction), systemImage: "point.3.connected.trianglepath.dotted")
                    .labelStyle(.titleAndIcon)
            }
            .help(model.localized(.workflowsHelp))
            .keyboardShortcut("w", modifiers: [.command, .option])
            .accessibilityIdentifier("workspace.workflows")

            Menu {
                Button(action: onOpenProject) {
                    Label(model.localized(.openAction), systemImage: "folder")
                }
                .keyboardShortcut("o", modifiers: [.command])

                Button(action: onSaveProject) {
                    Label(model.localized(.saveAction), systemImage: "square.and.arrow.down")
                }
                .keyboardShortcut("s", modifiers: [.command])
            } label: {
                Label(model.localized(.projectMenu), systemImage: "folder")
                    .labelStyle(.titleAndIcon)
            }
            .help(model.localized(.projectMenu))
            .accessibilityIdentifier("workspace.projectMenu")
            .disabled(model.hasActiveProcessing)
        }
    }
}

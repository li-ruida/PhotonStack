import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct LanguageSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        Picker(model.localized(.languageLabel), selection: $model.language) {
            ForEach(AppLanguage.allCases, id: \.self) { language in
                Text(language.displayName)
                    .tag(language)
            }
        }
        .pickerStyle(.segmented)
        .onChange(of: model.language) { _, _ in
            model.persistUserSettings()
        }
    }
}

struct NewProjectDraft: Equatable {
    var name: String
    var template: ProjectTemplate
    var reuseCurrentAssets = false

    var canCreate: Bool {
        name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false
    }
}

struct NewProjectSheet: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var draft: NewProjectDraft
    @FocusState private var isNameFocused: Bool
    let onCancel: () -> Void
    let onCreate: () -> Void

    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(model.localized(.newProject))
                .font(.title2.weight(.semibold))

            Text(chinese ? "为下一组作品建立独立工作区。" : "Start an independent workspace for your next image.")
                .foregroundStyle(.secondary)

            VStack(alignment: .leading, spacing: 10) {
                TextField(model.localized(.projectName), text: $draft.name)
                    .textFieldStyle(.roundedBorder)
                    .focused($isNameFocused)
                    .accessibilityIdentifier("project.new.name")
                    .onSubmit {
                        if draft.canCreate {
                            onCreate()
                        }
                    }

                Picker(model.localized(.templateLabel), selection: $draft.template) {
                    ForEach(ProjectTemplate.allCases, id: \.self) { projectTemplate in
                        Text(model.projectTemplateName(projectTemplate))
                            .tag(projectTemplate)
                    }
                }
                .pickerStyle(.menu)

                Picker(chinese ? "起点" : "Start with", selection: $draft.reuseCurrentAssets) {
                    Text(chinese ? "空白项目" : "Empty project").tag(false)
                    if model.project.assets.isEmpty == false {
                        Text(chinese ? "沿用当前素材（\(model.project.assets.count)）" : "Current assets (\(model.project.assets.count))")
                            .tag(true)
                    }
                }
                .pickerStyle(.radioGroup)
                .accessibilityIdentifier("project.new.source")

                Text(draft.reuseCurrentAssets
                    ? (chinese ? "只引用原始素材并保留帧角色；处理参数重新开始，不带入结果、图层或历史。" : "Reference the original files and keep their frame roles. Processing settings start fresh; results, layers and history stay in the current project.")
                    : (chinese ? "创建后可导入文件、文件夹或已堆栈的主图。" : "Import files, a folder or an existing master after creating the project."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if model.hasUnsavedChanges {
                Label(chinese ? "下一步将确认是否保存“\(model.project.name)”。" : "Next, choose whether to save “\(model.project.name)”.", systemImage: "doc.badge.clock")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            HStack(spacing: 10) {
                Button(model.localized(.cancelAction), action: onCancel)
                    .keyboardShortcut(.cancelAction)

                Spacer()

                Button(chinese ? "创建项目" : "Create project", action: onCreate)
                    .buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction)
                    .disabled(draft.canCreate == false || model.hasActiveProcessing)
                    .accessibilityIdentifier("project.new.create")
            }
        }
        .padding(22)
        .frame(width: 460)
        .onAppear { isNameFocused = true }
    }
}

struct ProjectSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.projectSection))
                .font(.headline)

            Picker(model.localized(.templateLabel), selection: $model.selectedTemplate) {
                ForEach(ProjectTemplate.allCases, id: \.self) { template in
                    Text(model.projectTemplateName(template))
                        .tag(template)
                }
            }
            .pickerStyle(.menu)
            .onChange(of: model.selectedTemplate) { _, _ in
                model.persistUserSettings()
            }

            Toggle(
                model.localized(.autosaveLabel),
                isOn: Binding(
                    get: { model.autosaveEnabled },
                    set: { model.setAutosaveEnabled($0) }
                )
            )

            if model.hasUnsavedChanges {
                Label(model.localized(.unsavedChangesStatus), systemImage: "circle.fill")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }

            VStack(alignment: .leading, spacing: 6) {
                Text(model.localized(.recentProjects))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                if model.recentProjects.isEmpty {
                    Text(model.localized(.noRecentProjects))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(model.recentProjects.prefix(5)) { recentProject in
                        Button {
                            model.openRecentProject(recentProject)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(recentProject.name)
                                    .lineLimit(1)
                                Text(recentProject.directory.path)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .buttonStyle(.plain)
                        .disabled(model.isProcessing)
                    }
                }
            }
        }
    }
}

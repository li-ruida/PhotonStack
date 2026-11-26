import SwiftUI

/// Task-oriented entry points keep stacking and finishing an existing master distinct.
struct WorkspaceWelcomeView: View {
    let chinese: Bool
    let onImport: () -> Void
    let onImportFolder: () -> Void
    let onEditMaster: () -> Void
    let onOpenProject: () -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 16) {
                        sequenceCard.frame(minWidth: 300)
                        masterCard.frame(minWidth: 260)
                    }
                    VStack(spacing: 16) { sequenceCard; masterCard }
                }

                Divider()
                HStack {
                    Spacer()
                    Button(chinese ? "打开项目…" : "Open project…", action: onOpenProject)
                        .accessibilityIdentifier("journey.open-project")
                }
            }
            .frame(maxWidth: 800, alignment: .leading)
            .padding(36)
            .frame(maxWidth: .infinity)
        }
        .defaultScrollAnchor(.center)
    }

    private var sequenceCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 16) {
                Label(chinese ? "堆栈一组照片" : "Stack a sequence", systemImage: "square.3.layers.3d")
                    .font(.title3.weight(.semibold))
                Text(chinese ? "同一天体 · 至少 3 张" : "One target · At least 3 frames")
                    .font(.callout).foregroundStyle(.secondary)
                HStack {
                    Button(action: onImport) { Label(chinese ? "导入照片…" : "Import photos…", systemImage: "plus") }
                        .buttonStyle(.borderedProminent).accessibilityIdentifier("journey.import-files")
                    Button(chinese ? "导入文件夹…" : "Import folder…", action: onImportFolder)
                        .accessibilityIdentifier("journey.import-folder")
                }
            }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var masterCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 16) {
                Label(chinese ? "调整已有成片" : "Finish an image", systemImage: "slider.horizontal.3")
                    .font(.title3.weight(.semibold))
                Text(chinese ? "单张照片或堆栈成片" : "Single photo or stacked image")
                    .font(.callout).foregroundStyle(.secondary)
                Button(chinese ? "选择图像…" : "Choose image…", action: onEditMaster)
                    .accessibilityIdentifier("journey.import-master")
            }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

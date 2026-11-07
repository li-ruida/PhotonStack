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
                VStack(alignment: .leading, spacing: 10) {
                    Label("PHOTONSTACK", systemImage: "sparkles")
                        .font(.caption.weight(.semibold)).tracking(2).foregroundStyle(.tint)
                    Text(chinese ? "让微弱的星光，逐渐清晰" : "Bring faint light into focus")
                        .font(.largeTitle.weight(.semibold))
                    Text(chinese ? "从拍摄序列开始，或继续调整已有成片。" : "Start with an image sequence, or finish an existing master.")
                        .foregroundStyle(.secondary)
                }

                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 16) {
                        sequenceCard.frame(minWidth: 300)
                        masterCard.frame(minWidth: 260)
                    }
                    VStack(spacing: 16) { sequenceCard; masterCard }
                }

                HStack(alignment: .top, spacing: 10) {
                    Image(systemName: "checkmark.shield").foregroundStyle(.secondary)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(chinese ? "原片保留，处理可追溯" : "Originals preserved. Processing traceable.")
                            .font(.callout.weight(.medium))
                        Text(chinese ? "支持 RAW、FITS、TIFF、JPEG、PNG、HEIF。导入和筛片不会修改原文件。" : "RAW, FITS, TIFF, JPEG, PNG, HEIF. Import and review leave source files unchanged.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                Divider()
                HStack {
                    Text(chinese ? "继续已有项目" : "Continue a project").foregroundStyle(.secondary)
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
                Text(chinese ? "导入同一目标的至少 3 张亮场，分析星点与噪声，再筛片、对齐与合成。" : "Import at least 3 lights of one target. Review stars and noise, then align and combine.")
                    .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Text(chinese ? "导入 → 筛片 → 堆栈 → 后期" : "Import → Review → Stack → Finish")
                    .font(.caption.weight(.medium)).foregroundStyle(.tint)
                HStack {
                    Button(action: onImport) { Label(chinese ? "导入照片…" : "Import photos…", systemImage: "plus") }
                        .buttonStyle(.borderedProminent).accessibilityIdentifier("journey.import-files")
                    Button(chinese ? "文件夹…" : "Folder…", action: onImportFolder)
                        .accessibilityIdentifier("journey.import-folder")
                }.controlSize(.large)
            }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var masterCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 16) {
                Label(chinese ? "调整已有成片" : "Finish an image", systemImage: "slider.horizontal.3")
                    .font(.title3.weight(.semibold))
                Text(chinese ? "打开已堆好的 FITS / TIFF 或单张照片，直接调整亮度、色彩与细节。" : "Open a FITS / TIFF master or a single photo to adjust brightness, color and detail.")
                    .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Text(chinese ? "导入 → 后期 → 导出" : "Import → Finish → Export")
                    .font(.caption.weight(.medium)).foregroundStyle(.secondary)
                Button(chinese ? "选择图像…" : "Choose image…", action: onEditMaster)
                    .controlSize(.large).accessibilityIdentifier("journey.import-master")
            }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

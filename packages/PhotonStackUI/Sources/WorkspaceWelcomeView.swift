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
                Text(chinese ? "开始处理照片" : "Start processing")
                    .font(.largeTitle.weight(.semibold))

                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 16) {
                        sequenceCard.frame(minWidth: 300)
                        masterCard.frame(minWidth: 260)
                    }
                    VStack(spacing: 16) { sequenceCard; masterCard }
                }

                HStack(alignment: .top, spacing: 10) {
                    Image(systemName: "checkmark.shield").foregroundStyle(.secondary)
                    Text(chinese ? "支持 RAW、FITS、TIFF、JPEG、PNG 和 HEIF。导入和筛片会保留原文件。"
                         : "Import RAW, FITS, TIFF, JPEG, PNG or HEIF. Importing and reviewing preserve your originals.")
                        .font(.caption).foregroundStyle(.secondary)
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
                Text(chinese ? "导入同一目标的至少 3 张亮场，分析星点与噪声，再筛片、对齐与合成。" : "Import at least 3 lights of one target. Review stars and noise, then align and combine.")
                    .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                HStack {
                    Button(action: onImport) { Label(chinese ? "导入照片…" : "Import photos…", systemImage: "plus") }
                        .buttonStyle(.borderedProminent).accessibilityIdentifier("journey.import-files")
                    Button(chinese ? "导入文件夹…" : "Import folder…", action: onImportFolder)
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
                Button(chinese ? "选择图像…" : "Choose image…", action: onEditMaster)
                    .controlSize(.large).accessibilityIdentifier("journey.import-master")
            }.padding(14).frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

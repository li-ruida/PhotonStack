import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct BatchQueueSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.batchQueueSection))
                .font(.headline)

            HStack {
                Button(model.localized(.queueStretch)) {
                    model.enqueueCurrent(.stretch)
                }
                Button(model.localized(.queueDenoise)) {
                    model.enqueueCurrent(.denoise)
                }
            }
            .buttonStyle(.bordered)
            .disabled(model.canEnqueueCurrentBatchItem == false || model.isBatchRunning)

            HStack {
                Button(model.localized(.queueSharpen)) {
                    model.enqueueCurrent(.sharpen)
                }
                Button(model.localized(.queueStars)) {
                    model.enqueueCurrent(.starReduce)
                }
            }
            .buttonStyle(.bordered)
            .disabled(model.canEnqueueCurrentBatchItem == false || model.isBatchRunning)

            HStack {
                Button(model.localized(.queueAllStretch)) {
                    model.enqueueAllAssets(.stretch)
                }
                Button(model.localized(.queueAllDenoise)) {
                    model.enqueueAllAssets(.denoise)
                }
            }
            .buttonStyle(.bordered)
            .disabled(model.canEnqueueAllAssets == false || model.isBatchRunning)

            HStack {
                Button(model.localized(.queueAllSharpen)) {
                    model.enqueueAllAssets(.sharpen)
                }
                Button(model.localized(.queueAllStars)) {
                    model.enqueueAllAssets(.starReduce)
                }
            }
            .buttonStyle(.bordered)
            .disabled(model.canEnqueueAllAssets == false || model.isBatchRunning)

            HStack {
                Button(model.localized(.startQueue)) {
                    model.startBatchQueue()
                }
                .disabled(model.batchQueueInputsAvailable == false || model.isProcessing)

                Button(model.isBatchPaused
                       ? (chinese ? "继续队列" : "Resume queue")
                       : (chinese ? "完成当前任务后暂停" : "Pause after active tasks")) {
                    if model.isBatchPaused {
                        model.resumeBatchQueue()
                    } else {
                        model.pauseBatchQueue()
                    }
                }
                .disabled(model.isBatchRunning == false)
                .help(chinese ? "已开始的任务会继续完成；暂停只阻止启动后续任务。" : "Active tasks finish normally. Pausing only prevents subsequent tasks from starting.")
                .accessibilityIdentifier("workspace.batch.pause-dispatch")
            }
            .buttonStyle(.bordered)

            if model.isBatchPaused {
                let active = model.batchQueue.filter { $0.status == .running }.count
                Text(active > 0
                     ? (chinese ? "后续任务已暂停，正在等待 \(active) 个运行中任务完成。" : "New tasks are paused; \(active) active tasks are still finishing.")
                     : (chinese ? "队列已暂停。点击“继续队列”处理剩余任务。" : "Queue paused. Resume to process the remaining tasks."))
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityIdentifier("workspace.batch.pause-status")
            }

            HStack {
                Button(model.localized(.retryFailed)) {
                    model.retryFailedBatchItems()
                }
                .disabled(model.batchQueue.contains { $0.status == .failed || $0.status == .cancelled } == false || model.isBatchRunning)

                Button(model.localized(.clearQueue)) {
                    model.clearBatchQueue()
                }
                .disabled(model.batchQueue.isEmpty || model.isBatchRunning)
            }
            .buttonStyle(.bordered)

            VStack(alignment: .leading, spacing: 6) {
                Picker(
                    model.localized(.batchOutputFormat),
                    selection: Binding(
                        get: { model.project.batchOutputFormat },
                        set: { model.setBatchOutputFormat($0) }
                    )
                ) {
                    ForEach(BatchOutputFormat.allCases) { format in
                        Text(model.batchOutputFormatName(format))
                            .tag(format)
                    }
                }
                .pickerStyle(.segmented)
                .disabled(model.isBatchRunning)

                Stepper(
                    "\(model.localized(.batchConcurrency)) \(model.project.batchMaxConcurrentTasks)",
                    value: Binding(
                        get: { model.project.batchMaxConcurrentTasks },
                        set: { model.setBatchMaxConcurrentTasks($0) }
                    ),
                    in: 1...4
                )
                .disabled(model.isBatchRunning)

                HStack {
                    Text(model.localized(.batchOutputDirectory))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Spacer()

                    #if os(macOS)
                    Button {
                        if let directory = NativeFilePanels.openBatchOutputDirectory(
                            message: model.localized(.chooseBatchOutputDirectoryMessage)
                        ) {
                            model.setBatchOutputDirectory(directory)
                        }
                    } label: {
                        Label(model.localized(.chooseOutputDirectory), systemImage: "folder.badge.plus")
                            .labelStyle(.iconOnly)
                    }
                    .help(model.localized(.chooseOutputDirectory))
                    .disabled(model.isBatchRunning)

                    Button {
                        model.clearBatchOutputDirectory()
                    } label: {
                        Label(model.localized(.clearOutputDirectory), systemImage: "xmark.circle")
                            .labelStyle(.iconOnly)
                    }
                    .help(model.localized(.clearOutputDirectory))
                    .disabled(model.project.batchOutputDirectory == nil || model.isBatchRunning)
                    #endif

                }

                Text(model.project.batchOutputDirectory?.path ?? model.localized(.previewCache))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            Divider()

            if model.batchQueue.isEmpty {
                Text(model.localized(.noQueuedTasks))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                ForEach(model.batchQueue.suffix(8).reversed()) { item in
                    HStack(spacing: 8) {
                        Image(systemName: iconName(for: item.status))
                            .foregroundStyle(color(for: item.status))
                            .frame(width: 18)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(item.title)
                                .lineLimit(1)
                            Text(item.inputURL.lastPathComponent)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                        Spacer()
                        if item.attempts > 0 {
                            Text("\(item.attempts)")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .font(.caption)
                    .padding(.vertical, 2)
                }
            }
        }
    }

    private func iconName(for status: ProcessingJobStatus) -> String {
        switch status {
        case .queued:
            return "clock"
        case .running:
            return "progress.indicator"
        case .succeeded:
            return "checkmark.circle.fill"
        case .failed:
            return "xmark.octagon.fill"
        case .cancelled:
            return "minus.circle"
        }
    }

    private func color(for status: ProcessingJobStatus) -> Color {
        switch status {
        case .queued, .cancelled:
            return .secondary
        case .running:
            return .blue
        case .succeeded:
            return .green
        case .failed:
            return .red
        }
    }
}

import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct JobsSection: View {
    let jobs: [ProcessingJob]
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.tasksSection))
                .font(.headline)

            if jobs.isEmpty {
                Text(model.localized(.noTasksYet))
                    .foregroundStyle(.secondary)
            } else {
                ForEach(jobs.suffix(8).reversed()) { job in
                    HStack(spacing: 8) {
                        Image(systemName: iconName(for: job.status))
                            .foregroundStyle(color(for: job.status))
                            .frame(width: 18)

                        VStack(alignment: .leading, spacing: 2) {
                            Text(job.title)
                                .lineLimit(1)
                            if let outputURL = job.outputURL {
                                Text(outputURL.lastPathComponent)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }
                            HStack(spacing: 6) {
                                if job.cacheHit {
                                    Text(model.localized(.cacheHitLabel))
                                        .foregroundStyle(.green)
                                } else if job.status == .succeeded {
                                    Text(model.localized(.executedLabel))
                                        .foregroundStyle(.blue)
                                }
                                if let duration = job.durationMilliseconds {
                                    Text(formatDuration(duration))
                                        .foregroundStyle(.secondary)
                                }
                            }
                            .font(.caption2)
                        }

                        Spacer()
                    }
                    .font(.caption)
                    .padding(.vertical, 3)
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

    private func formatDuration(_ milliseconds: Double) -> String {
        if milliseconds < 1000 {
            return "\(Int(milliseconds.rounded())) ms"
        }
        return String(format: "%.2f s", milliseconds / 1000)
    }
}

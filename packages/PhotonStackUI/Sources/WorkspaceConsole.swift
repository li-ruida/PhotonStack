import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct StatusConsole: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var commandInput = ""
    @AppStorage("PhotonStackStatusConsole.isCollapsed") private var isCollapsed = true
    @AppStorage("PhotonStackStatusConsole.isExpanded") private var isExpanded = false
    #if os(macOS)
    @State private var detachedWindow: NSWindow?
    #endif

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(model.isProcessing ? model.localized(.processingStatus) : model.localized(.consoleStatus))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Spacer()

                if model.isProcessing, let progress = model.progressFraction {
                    Text("\(Int((progress * 100).rounded()))%")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }

                if let errorMessage = model.errorMessage {
                    let localizedError = model.localizedMessage(errorMessage)
                    Button {
                        withAnimation(.easeInOut(duration: 0.18)) {
                            isCollapsed = false
                        }
                    } label: {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundStyle(.red)
                    }
                    .buttonStyle(.borderless)
                    .frame(width: 28, height: 24)
                    .help(localizedError)
                    .accessibilityLabel(Text(localizedError))
                }

                Button {
                    withAnimation(.easeInOut(duration: 0.18)) {
                        isCollapsed.toggle()
                        if isCollapsed {
                            isExpanded = false
                        }
                    }
                } label: {
                    Label(
                        isCollapsed ? model.localized(.consoleExpand) : model.localized(.consoleCollapse),
                        systemImage: isCollapsed ? "chevron.up" : "chevron.down"
                    )
                    .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .frame(width: 28, height: 24)
                .help(isCollapsed ? model.localized(.consoleExpand) : model.localized(.consoleCollapse))

                if isCollapsed == false {
                    Button {
                        withAnimation(.easeInOut(duration: 0.18)) {
                            isExpanded.toggle()
                        }
                    } label: {
                        Label(
                            isExpanded ? model.localized(.consoleZoomOut) : model.localized(.consoleZoomIn),
                            systemImage: isExpanded ? "arrow.down.right.and.arrow.up.left" : "arrow.up.left.and.arrow.down.right"
                        )
                        .labelStyle(.iconOnly)
                    }
                    .buttonStyle(.borderless)
                    .frame(width: 28, height: 24)
                    .help(isExpanded ? model.localized(.consoleZoomOut) : model.localized(.consoleZoomIn))
                }

                #if os(macOS)
                Button {
                    openDetachedConsole()
                } label: {
                    Label(model.localized(.consolePopOut), systemImage: "macwindow")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .frame(width: 28, height: 24)
                .help(model.localized(.consolePopOut))
                #endif
            }

            if isCollapsed == false {
                if model.isProcessing {
                    ProgressView(value: model.progressFraction ?? 0)
                        .controlSize(.small)
                    if let progressMessage = model.progressMessage {
                        Text(progressMessage)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }

                HStack(spacing: 8) {
                    Text(">")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)

                    TextField(model.localized(.consoleInputPlaceholder), text: $commandInput)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.caption, design: .monospaced))
                        .disabled(model.isProcessing)
                        .onSubmit {
                            submitCommand()
                        }

                    Button {
                        submitCommand()
                    } label: {
                        Label(model.localized(.consoleRunCommand), systemImage: "terminal")
                    }
                    .help(model.localized(.consoleRunCommand))
                    .disabled(model.isProcessing || commandInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }

                if let errorMessage = model.errorMessage {
                    Text(model.localizedMessage(errorMessage))
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.red)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                } else {
                    ScrollView {
                        Text(model.commandOutput.isEmpty ? model.localized(.readyStatus) : model.commandOutput)
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(model.commandOutput.isEmpty ? .secondary : .primary)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
        }
        .padding(12)
        .frame(height: consoleHeight, alignment: .top)
        .clipped()
    }

    private var consoleHeight: CGFloat {
        if isCollapsed {
            return 48
        }
        return isExpanded ? 420 : 220
    }

    private func submitCommand() {
        let command = commandInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard command.isEmpty == false else {
            return
        }
        model.startRunConsoleCommand(command)
    }

    #if os(macOS)
    private func openDetachedConsole() {
        if let detachedWindow, detachedWindow.isVisible {
            detachedWindow.makeKeyAndOrderFront(nil)
            return
        }

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 860, height: 560),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = model.localized(.consoleStatus)
        window.isReleasedWhenClosed = false
        window.contentViewController = NSHostingController(rootView: DetachedConsoleWindow(model: model))
        window.setFrameAutosaveName("PhotonStackDetachedConsole")
        window.center()
        window.makeKeyAndOrderFront(nil)
        detachedWindow = window
    }
    #endif
}

private struct DetachedConsoleWindow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var commandInput = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(model.isProcessing ? model.localized(.processingStatus) : model.localized(.consoleStatus))
                    .font(.headline)

                Spacer()

                if model.isProcessing, let progress = model.progressFraction {
                    Text("\(Int((progress * 100).rounded()))%")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }

            if model.isProcessing {
                ProgressView(value: model.progressFraction ?? 0)
                    .controlSize(.small)
                if let progressMessage = model.progressMessage {
                    Text(progressMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }

            HStack(spacing: 8) {
                Text(">")
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(.secondary)

                TextField(model.localized(.consoleInputPlaceholder), text: $commandInput)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(.body, design: .monospaced))
                    .disabled(model.isProcessing)
                    .onSubmit {
                        submitCommand()
                    }

                Button {
                    submitCommand()
                } label: {
                    Label(model.localized(.consoleRunCommand), systemImage: "terminal")
                }
                .disabled(model.isProcessing || commandInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }

            ConsoleOutputView(model: model)
        }
        .padding(16)
        .frame(minWidth: 720, minHeight: 460)
    }

    private func submitCommand() {
        let command = commandInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard command.isEmpty == false else {
            return
        }
        model.startRunConsoleCommand(command)
    }
}

private struct ConsoleOutputView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        if let errorMessage = model.errorMessage {
            ScrollView {
                Text(model.localizedMessage(errorMessage))
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.red)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        } else {
            ScrollView {
                Text(model.commandOutput.isEmpty ? model.localized(.readyStatus) : model.commandOutput)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(model.commandOutput.isEmpty ? .secondary : .primary)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }
}

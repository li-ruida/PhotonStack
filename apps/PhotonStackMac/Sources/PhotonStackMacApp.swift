import AppKit
import OSLog
import PhotonStackProcessing
import PhotonStackUI
import SwiftUI

private let photonStackMacLogger = Logger(subsystem: "dev.photonstack.app", category: "mac")

@main
struct PhotonStackMacApp: App {
    @NSApplicationDelegateAdaptor(PhotonStackMacAppDelegate.self) private var appDelegate
    @StateObject private var commandState = PhotonStackApplicationCommandState()

    var body: some Scene {
        Window("PhotonStack", id: "main") {
            PhotonStackRootView(
                processingService: CLIProcessingService.makeDefault(),
                applicationCommandState: commandState
            )
                .frame(minWidth: 1120, minHeight: 720)
                .onAppear {
                    NSApp.setActivationPolicy(.regular)
                    NSApp.activate(ignoringOtherApps: true)
                }
        }
        .commands {
            PhotonStackProcessCommands(state: commandState)
        }
    }
}

@MainActor
final class PhotonStackMacAppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
        let launchURLs = launchArgumentURLs()
        photonStackMacLogger.notice(
            "Application finished launching with \(launchURLs.count, privacy: .public) launch argument URL(s)"
        )
        importExternalFiles(launchURLs)
    }

    func application(_ sender: NSApplication, openFile filename: String) -> Bool {
        photonStackMacLogger.notice("Received openFile event for \(filename, privacy: .public)")
        importExternalFiles([URL(fileURLWithPath: filename)])
        return true
    }

    func application(_ sender: NSApplication, openFiles filenames: [String]) {
        photonStackMacLogger.notice(
            "Received openFiles event for \(filenames.count, privacy: .public) file(s)"
        )
        importExternalFiles(filenames.map { URL(fileURLWithPath: $0) })
        sender.reply(toOpenOrPrint: .success)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        let hasUnsavedDocument = sender.windows.contains(where: { $0.isDocumentEdited })
        let hasActiveProcessing = PhotonStackApplicationActivity.hasActiveProcessing
        if hasUnsavedDocument == false,
           hasActiveProcessing == false,
           PhotonStackApplicationActivity.hasPendingSettingsPersistence {
            Task { @MainActor in
                await PhotonStackApplicationActivity.waitForPendingSettingsPersistence()
                sender.reply(toApplicationShouldTerminate: true)
            }
            return .terminateLater
        }
        guard hasUnsavedDocument || hasActiveProcessing else {
            return .terminateNow
        }
        NotificationCenter.default.post(name: .photonStackRequestApplicationTermination, object: nil)
        return .terminateLater
    }

    func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool {
        false
    }

    func applicationShouldRestoreApplicationState(_ app: NSApplication) -> Bool {
        false
    }

    func applicationShouldSaveApplicationState(_ app: NSApplication) -> Bool {
        false
    }

    private func importExternalFiles(_ urls: [URL]) {
        guard urls.isEmpty == false else {
            photonStackMacLogger.notice("Skipping external import because no URLs were provided")
            return
        }

        NSApp.activate(ignoringOtherApps: true)
        photonStackMacLogger.notice("Queueing \(urls.count, privacy: .public) external import URL(s)")
        PhotonStackApplicationActivity.enqueueExternalFileImports(urls)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            photonStackMacLogger.notice("Posting import notification for \(urls.count, privacy: .public) URL(s)")
            NotificationCenter.default.post(name: .photonStackImportAssetURLs, object: urls)
        }
    }

    private func launchArgumentURLs() -> [URL] {
        CommandLine.arguments.dropFirst().compactMap { argument in
            let url = URL(fileURLWithPath: argument)
            guard FileManager.default.fileExists(atPath: url.path) else {
                return nil
            }
            return url
        }
    }
}

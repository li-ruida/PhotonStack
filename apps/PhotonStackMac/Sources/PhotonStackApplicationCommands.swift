import AppKit
import PhotonStackAppCore
import PhotonStackUI
import SwiftUI

struct PhotonStackApplicationCommands: Commands {
    @ObservedObject var state: PhotonStackApplicationCommandState
    @Environment(\.openSettings) private var openSettings

    private var language: AppLanguage {
        state.snapshot.language
    }

    var body: some Commands {
        CommandGroup(replacing: .appInfo) {
            Button(text("关于 PhotonStack", "About PhotonStack")) {
                NSApp.orderFrontStandardAboutPanel(nil)
            }
        }

        CommandGroup(replacing: .appSettings) {
            Button(text("设置…", "Settings…")) {
                openSettings()
            }
            .keyboardShortcut(",", modifiers: [.command])
        }

        CommandGroup(replacing: .appVisibility) {
            Button(text("隐藏 PhotonStack", "Hide PhotonStack")) {
                NSApp.hide(nil)
            }
            .keyboardShortcut("h", modifiers: [.command])

            Button(text("隐藏其他", "Hide Others")) {
                NSApp.hideOtherApplications(nil)
            }
            .keyboardShortcut("h", modifiers: [.command, .option])

            Button(text("全部显示", "Show All")) {
                NSApp.unhideAllApplications(nil)
            }
        }

        CommandGroup(replacing: .appTermination) {
            Button(text("退出 PhotonStack", "Quit PhotonStack")) {
                NSApp.terminate(nil)
            }
            .keyboardShortcut("q", modifiers: [.command])
        }

        CommandGroup(replacing: .help) {}
    }

    private func text(_ chinese: String, _ english: String) -> String {
        language == .simplifiedChinese ? chinese : english
    }
}

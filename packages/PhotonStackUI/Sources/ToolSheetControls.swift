import SwiftUI

/// Keep task status on the left and the final action on the right.
struct ToolActionBar<Content: View>: View {
    private let content: Content
    init(@ViewBuilder content: () -> Content) { self.content = content() }
    var body: some View {
        VStack(spacing: 10) {
            Divider()
            HStack(spacing: 10) { content }
                .frame(minHeight: 28)
                .controlSize(.regular)
                .buttonStyle(.bordered)
        }
    }
}

struct ToolSheetCloseButton: View {
    let chinese: Bool
    let busy: Bool
    let action: () -> Void
    var body: some View {
        Button(chinese ? "关闭" : "Close", action: action)
            .keyboardShortcut(.cancelAction)
            .disabled(busy)
    }
}

struct ToolTaskStatus: View {
    let title: String
    let chinese: Bool
    var cancelIdentifier = "tool.cancel"
    var cancel: (() -> Void)? = nil
    var body: some View {
        HStack(spacing: 8) {
            ProgressView().controlSize(.small)
            Text(title).font(.caption).foregroundStyle(.secondary)
            if let cancel {
                Button(chinese ? "取消任务" : "Cancel task", action: cancel)
                    .keyboardShortcut(".", modifiers: .command)
                    .accessibilityIdentifier(cancelIdentifier)
            }
        }
    }
}

extension View {
    /// Command-Return submits; Return remains available for numeric field editing.
    func toolPrimaryAction() -> some View {
        buttonStyle(.borderedProminent)
            .keyboardShortcut(.return, modifiers: .command)
    }
}

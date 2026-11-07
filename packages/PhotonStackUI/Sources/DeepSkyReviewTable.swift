import AppKit
import PhotonStackAppCore
import SwiftUI

/// A flat native table. Changing filters atomically replaces the data source
/// and reapplies selection by path, even when the old rows no longer exist.
struct DeepSkyReviewTable: NSViewRepresentable {
    struct Row {
        let index: Int
        let filename, decision, symbol: String
        let kept: Bool
    }
    struct ContentKey: Equatable {
        let report: DeepSkyReport
        let overrides: [String: String]
        let apply: String?
        let current, chinese: Bool
    }
    let paths: [String]
    let content: ContentKey
    let selected: Set<String>
    let focused: String?
    let row: (Int) -> Row
    let select: (Set<String>) -> Void

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.documentView = context.coordinator.table
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        let coordinator = context.coordinator
        let changed = coordinator.content != content
        coordinator.content = content
        coordinator.update(paths: paths, selected: selected, focused: focused, redraw: changed, row: row, select: select)
    }

    @MainActor
    final class Coordinator: NSObject, NSTableViewDataSource, NSTableViewDelegate {
        let table = NSTableView()
        var content: ContentKey?
        private var paths: [String] = []
        private var rowAt: ((Int) -> Row)?
        private var select: ((Set<String>) -> Void)?
        private var applying = false
        private var previousFocus: String?

        override init() {
            super.init()
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("exposure"))
            column.resizingMask = .autoresizingMask
            table.addTableColumn(column)
            table.columnAutoresizingStyle = .lastColumnOnlyAutoresizingStyle
            table.headerView = nil
            table.allowsMultipleSelection = true
            table.allowsEmptySelection = true
            table.style = .inset
            table.rowHeight = 44
            table.dataSource = self
            table.delegate = self
            table.identifier = NSUserInterfaceItemIdentifier("review.frames")
            table.setAccessibilityIdentifier("review.frames")
        }

        func update(paths newPaths: [String], selected: Set<String>, focused: String?, redraw: Bool,
                    row: @escaping (Int) -> Row, select: @escaping (Set<String>) -> Void) {
            applying = true
            defer { applying = false }
            let changed = paths != newPaths
            paths = newPaths
            rowAt = row
            self.select = select
            if changed || redraw { table.reloadData() }
            let indexes = IndexSet(paths.indices.filter { selected.contains(paths[$0]) })
            if table.selectedRowIndexes != indexes { table.selectRowIndexes(indexes, byExtendingSelection: false) }
            if let previousFocus, focused != previousFocus, let focused, let index = paths.firstIndex(of: focused) {
                table.scrollRowToVisible(index)
            }
            previousFocus = focused
        }

        func numberOfRows(in tableView: NSTableView) -> Int { paths.count }

        func tableViewSelectionDidChange(_ notification: Notification) {
            guard !applying else { return }
            select?(Set(table.selectedRowIndexes.compactMap { paths.indices.contains($0) ? paths[$0] : nil }))
        }

        func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row index: Int) -> NSView? {
            guard paths.indices.contains(index), let data = rowAt?(index) else { return nil }
            let id = NSUserInterfaceItemIdentifier("review-frame")
            let cell = tableView.makeView(withIdentifier: id, owner: nil) as? FrameCell ?? FrameCell(frame: .zero)
            cell.identifier = id
            cell.update(data, path: paths[index])
            return cell
        }
    }

    @MainActor
    private final class FrameCell: NSTableCellView {
        let name = NSTextField(labelWithString: "")
        let decision = NSTextField(labelWithString: "")
        let icon = NSImageView()

        override init(frame: NSRect) {
            super.init(frame: frame)
            name.lineBreakMode = .byTruncatingMiddle
            decision.lineBreakMode = .byTruncatingTail
            decision.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            decision.textColor = .secondaryLabelColor
            let labels = NSStackView(views: [name, decision])
            labels.orientation = .vertical
            labels.alignment = .leading
            labels.spacing = 1
            for label in [name, decision] {
                label.maximumNumberOfLines = 1
                label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            }
            for view in [icon, labels] as [NSView] {
                view.translatesAutoresizingMaskIntoConstraints = false
                addSubview(view)
            }
            NSLayoutConstraint.activate([
                icon.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
                icon.topAnchor.constraint(equalTo: labels.topAnchor, constant: 1),
                icon.widthAnchor.constraint(equalToConstant: 14), icon.heightAnchor.constraint(equalToConstant: 14),
                labels.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 6),
                labels.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
                labels.centerYAnchor.constraint(equalTo: centerYAnchor),
            ])
            textField = name
            imageView = icon
        }
        required init?(coder: NSCoder) { return nil }

        func update(_ row: Row, path: String) {
            name.stringValue = "#\(row.index + 1)  " + row.filename
            decision.stringValue = row.decision
            icon.image = NSImage(systemSymbolName: row.symbol, accessibilityDescription: nil)
            icon.contentTintColor = row.kept ? .systemGreen : .systemOrange
            toolTip = path + "\n" + row.decision
            setAccessibilityIdentifier("review.frame.\(row.index)")
            setAccessibilityLabel(name.stringValue + ", " + row.decision)
        }
    }
}

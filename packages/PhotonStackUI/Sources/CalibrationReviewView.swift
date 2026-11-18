import SwiftUI

struct CalibrationReviewView: View {
    let report: CalibrationPlanReport
    let chinese: Bool
    let masterLabels: [String: String]
    @Binding var groupSelection: Int?
    @Binding var frameSelection: Int?
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    private var group: CalibrationPlanReport.Group? {
        guard let groupSelection, report.groups.indices.contains(groupSelection) else { return nil }
        return report.groups[groupSelection]
    }
    var body: some View {
        HSplitView {
            VStack(alignment: .leading, spacing: 8) {
                Text(t("素材分组", "Acquisition groups")).font(.headline)
                List(report.groups.indices, id: \.self, selection: $groupSelection) { i in
                    let g = report.groups[i]
                    VStack(alignment: .leading, spacing: 3) {
                        Text((g.text["object"] ?? t("未命名目标", "Unknown target")) + " · " + (g.text["filter"] ?? "?"))
                        Text("\(g.members.count) " + t("张 · ", "frames · ") + (g.text["utcDate"] ?? "?") + " · " +
                            (g.numeric["exposure"].map { $0.formatted() + " s" } ?? t("曝光未知", "Unknown exposure")))
                            .font(.caption).foregroundStyle(.secondary)
                    }.tag(i)
                }.listStyle(.bordered).frame(minHeight: 130, maxHeight: 200).accessibilityIdentifier("calibration.groups")
                if let group {
                    Text(t("组内原片", "Frames in group")).font(.headline)
                    List(group.members, id: \.self, selection: $frameSelection) { i in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(URL(fileURLWithPath: report.lights[i].path).lastPathComponent).lineLimit(1)
                            Text(CalibrationPresentation.value("sensorTemperature", in: report.lights[i], chinese: chinese) + " °C")
                                .font(.caption).foregroundStyle(.secondary)
                        }.tag(i)
                    }.listStyle(.bordered).accessibilityIdentifier("calibration.frames")
                }
            }.frame(minWidth: 230, idealWidth: 270, maxWidth: 320).padding(.trailing, 10)
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    if let group {
                        Text(t("整组候选", "Candidates for the entire group")).font(.headline)
                        choices(group.roles)
                        if !group.issues.isEmpty {
                            Text(t("此组缺少可靠的目标或日期信息，或文件头有冲突；请核对原片。", "Check this group's target, date and header conflicts."))
                                .foregroundStyle(.orange)
                        }
                        if let frameSelection, group.members.contains(frameSelection) {
                            Divider()
                            Text(t("当前原片", "Selected frame")).font(.headline)
                            Text(URL(fileURLWithPath: report.lights[frameSelection].path).lastPathComponent).textSelection(.enabled)
                            choices(report.frameChoices[frameSelection].roles)
                            DisclosureGroup(t("查看拍摄参数", "Acquisition parameters")) {
                                CalibrationAcquisitionDetails(source: report.lights[frameSelection], chinese: chinese)
                            }
                        }
                        Divider()
                        Text(t("组内匹配依据", "Matching evidence for this group")).font(.headline)
                        ForEach(group.candidates.indices, id: \.self) { i in
                            let c = group.candidates[i], master = report.masters[c.master]
                            VStack(alignment: .leading, spacing: 5) {
                                Text(CalibrationPresentation.role(master.role, chinese: chinese) + " · " + label(c.master))
                                    .font(.subheadline.bold())
                                Text("\(t("匹配", "Match")) \(c.compatibleCount) · \(t("待核对", "Review")) \(c.reviewCount) · \(t("冲突", "Conflict")) \(c.incompatibleCount)")
                                    .foregroundStyle(c.status == "compatible" ? Color.secondary : Color.orange)
                                if !c.issues.isEmpty {
                                    DisclosureGroup(t("组内异常示例", "Group issue examples")) {
                                        ForEach(c.issues.indices, id: \.self) { j in
                                            let issue = c.issues[j], e = issue.example
                                            VStack(alignment: .leading, spacing: 2) {
                                                Text(CalibrationPresentation.field(e.field, chinese: chinese) + " · " + CalibrationPresentation.status(e.status, chinese: chinese) + " · \(issue.count) " + t("张", "frames"))
                                                Text("\(t("原片", "Input")): \(e.input.isEmpty ? "?" : e.input)  →  \(t("母片", "Master")): \(e.master.isEmpty ? "?" : e.master)")
                                                Text(URL(fileURLWithPath: report.lights[issue.firstMember].path).lastPathComponent).lineLimit(1)
                                            }.font(.caption).foregroundStyle(.secondary).textSelection(.enabled).padding(.vertical, 3)
                                        }
                                    }.accessibilityIdentifier("calibration.candidate-issues.\(c.master)")
                                }
                            }.padding(.vertical, 5)
                        }
                    }
                }.frame(maxWidth: .infinity, alignment: .leading).padding(.leading, 12)
            }
        }.onChange(of: groupSelection) { _, _ in frameSelection = group?.members.first }
    }
    private func choices(_ roles: [String: CalibrationPlanReport.Choice]) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            ForEach(CalibrationPlanReport.roles, id: \.self) { role in
                if let choice = roles[role] {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(CalibrationPresentation.role(role, chinese: chinese) + "：" + CalibrationPresentation.status(choice.status, chinese: chinese))
                            .foregroundStyle(choice.status == "unique-compatible" ? Color.primary : Color.orange)
                        if !choice.compatibleMasters.isEmpty {
                            Text(choice.compatibleMasters.map(label).joined(separator: " · ")).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
        }.font(.callout)
    }
    private func label(_ index: Int) -> String {
        masterLabels[report.masters[index].acquisition.sha] ?? CalibrationPresentation.role(report.masters[index].role, chinese: chinese)
    }
}

struct CalibrationAcquisitionDetails: View {
    let source: CalibrationAcquisition
    let chinese: Bool
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("\(source.width) × \(source.height) · \(source.channels) " + (chinese ? "通道" : "channels"))
            Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 5) {
                ForEach(CalibrationPresentation.fields, id: \.self) { field in
                    GridRow {
                        Text(CalibrationPresentation.field(field, chinese: chinese)).foregroundStyle(.secondary)
                        Text(CalibrationPresentation.value(field, in: source, chinese: chinese)).textSelection(.enabled)
                    }
                }
            }.font(.callout)
            ForEach(source.issues.indices, id: \.self) { i in
                let issue = source.issues[i]
                Text(CalibrationPresentation.field(issue.field, chinese: chinese) + ": " + issue.message).font(.caption).foregroundStyle(.orange)
            }
        }.frame(maxWidth: .infinity, alignment: .leading).padding(.top, 6)
    }
}

# 2026-11-27 缺陷复核后的修复

后续复判与补漏见 [2026-11-28 第二轮记录](bug-audit-second-pass-2026-11-28.md)，其中 H7 已确认条件性缺陷并修复。下文保留本轮当时的结论和验证范围。

基于 `bug-audit` 分支的 `1e5c237`。编号对应审计记录；修复范围以复核证据为准，没有把全部 29 条都认定为用户可触发故障。

## 已处理

| 原编号 | 修改后的行为 |
|---|---|
| H1 | 星点检测拒绝 `border=0`，图像和亮度数组入口均不再越界。 |
| H2 | 检测和使用外部轨迹的移除路径共用图像/参数校验；拒绝畸形缓冲区、非有限像素、非法半径和轨迹几何；蒙版范围先裁到图像内再转整数。 |
| H3 | sensor-master 和 calibration-run 接受新输出目录的尾部斜杠。 |
| H4 | 空 solver.log 不再污染 stderr 的流状态，求解失败仍输出 JSON 错误。 |
| H5 | 拼接目录统一按标准化路径去重，目录 URL 的尾斜杠提示差异不再造成字典重复键崩溃。 |
| H6 | 移除素材、改变角色会清理其校准引用和 selection override，失效旧报告；保留其他显式外部母片，并将更改后的素材加入已有的目标角色列表。 |
| H8 | UI 配方导入纳入前台任务跟踪；完成和失败回写均检查项目 ID、导入代次及取消状态。 |
| M1 | 配方 RAW 曝光范围统一为解码器支持的 [-5, 5]；保留堆栈对非法配置的统一校验契约。 |
| M2 | 不可逆投影/无效分母返回非有限坐标，采样按无覆盖处理，不再伪装成恒等变换。 |
| M4 | 三个临时并行循环改用自动 join 的 jthread，后续线程创建失败不会销毁未 join 的线程。 |
| M5 | 曲线 CLI、服务和重放传递相同 RAW 参数，新操作持久化解码设置；桥接拒绝非有限数值，正确报告空曲线，并暂存输出、检查取消后原子提交。 |
| M6 | mosaic 拒绝 columns/alignment-width=0；preview-width=0 保留全分辨率语义并更新诊断文案。 |
| M7 | SPCC 与 OpenNGC 导入将浮点舍入后的 RA=360 重新归零，保持 [0,360) 契约。 |
| M8 | 派生成片按文件身份查找所属素材，选中结果后不再增加同一图片的无效回退记录。 |
| M9 | GHS/背景面板跟踪打开和保存任务；关闭时取消并等待任务后清理临时目录；背景复制传播取消；保存后检查项目身份再使用结果。 |
| L1 | 天文测量提取明确拒绝非有限 alpha 覆盖。 |
| L2 | Core Image 上下文创建也纳入色彩空间释放的 finally 范围。 |
| L3 | review-preview 的原子提交保留“仅限新目标”契约，已有/并发创建的目标不会被替换。 |
| L5 | 工作流在执行任何步骤前拒绝未闭合引号。 |
| L6 | 打包图标改用 macOS 自带 sips/iconutil，移除 Pillow 依赖。 |
| L7 | deep-sky 异常增加通用 code 字段，同时保留 errorCode 兼容旧调用者；区分参数错误与运行失败。 |
| L8 | CLI JSON 完整转义 ASCII 控制字符。 |
| L9 | 面板取消不再显示为普通操作失败。 |
| L10 | 旧 runStackWorkflow 的校准/RAW 解码输出包含素材 UUID，跨目录同名帧保持独立；当前 UI 没有直接调用此旧入口。 |
| L11 | 配方、预设、产品清单和校准审查导出也校验受保护输入路径。 |

## 未修改

- **H7**：未复现“关闭后退出卡死”，窗口保留及通知接收的前提仍需真实 App 生命周期验证。
- **M3**：撤回“极小 sigma 导致全图 NaN”的结论；复核实际输出有限且不变。
- **M10**：高级工具设置哪些应该跨项目/检查器页面保存，属于尚未明确的生命周期约定，本次不擅自重置或持久化全部设置。
- **L4**：保留 crop 手动矩形与自动比例模式的现有优先级，没有把尚未明确的参数混用约定当作算法错误。

## 验证

- CMake Debug 构建通过。完整 CTest **82/82 通过**，包括 Metal 工作流。
- 随后补充的坐标导入测试通过，更新后的引擎边界测试也通过；当前注册的 **83 个 CTest 项均已验证通过**。
- 完整 Swift 测试 **632 项：615 通过、17 跳过、0 失败**。跳过项由现有环境条件控制，涉及真实 M31 数据、Gaia、外部求解器和专门集成测试。
- 原生图标生成通过，ICNS 长度和格式检查通过，回解为 iconset 得到 10 个 PNG 表示；未执行整个打包或安装流程。
- Python 脚本语法、打包脚本 `bash -n`、`git diff --check` 通过。完整 SPCC 数据导入的可选 Astropy 测试未运行；此次 RA 修正通过独立 proper-motion 和 OpenNGC 导入边界测试验证。

主要命令：

```sh
cmake --build build/debug -j 4
ctest --test-dir build/debug -j 4 --output-on-failure
PHOTONSTACK_TEST_CLI="$PWD/build/debug/apps/PhotonStackCLI/photonstack" \
PHOTONSTACK_TEST_SCRATCH_PATH=/private/tmp/PhotonStackAuditSwiftBuild \
  tools/scripts/test-swift.sh --build-system native
python3 tests/cli/test_coordinate_wrapping.py
bash -n tools/scripts/package-macos.sh
```

新增测试位于各 Swift 模块的 `AuditRegressionTests.swift`、`tests/engine/AuditBoundaryTests.cpp`、`tests/cli/test_audit_regressions.py` 和 `tests/cli/test_coordinate_wrapping.py`；旧堆栈同名帧回归位于 `PhotonStackWorkspaceModelTests.swift`。

未切换分支、未提交或推送、未安装应用。原有的 `__pycache__` 未清理。

## 同类问题追修

- **输出覆盖契约**：将 `denoise-nonlocal` 和带 `--statistics-reference` 的 `develop` 纳入原子提交的“仅限新目标”规则。命令规则集中到 `requiresNewOutput`，提交继续使用 `RENAME_EXCL`，同时保护开始前已有和运行期间出现的文件。普通 `develop` 保留原来的可替换输出行为。
- **参考帧与逐帧权重**：素材增删、角色变更时，根据变更前后亮场的素材 UUID 重映射位置参数。保留参考帧身份；参考被移除时回退自动选择；现有权重随素材保留，新加入亮场默认权重为 1；空权重仍保持等权。原本长度不匹配的列表不猜测绑定关系，保留原值。
- **批量目标去重**：工作流按解析符号链接后的规范路径字符串去重，并统一构造目录 URL；同一目录的不同 `isDirectory` 表示和符号链接别名只产生一组任务。保留本地文件 URL 限制。

对应回归覆盖：已有/并发创建输出的保留、真实 CLI 服务新建成功及覆盖拒绝、普通 develop 可替换输出、素材删除/改类/增加/重复添加、参考帧移除、项目编码解码、生成配方的真实引擎验证，以及批量工作流执行次数与原片不变。

本轮 Swift 构建通过；相关回归共 **221 项：214 通过、7 因真实数据或专用环境条件跳过、0 失败**。`git diff --check` 通过。本轮改动为 Swift 与文档，没有重复执行上一轮完整 CTest，也没有提交、推送或安装应用。

```sh
PHOTONSTACK_TEST_CLI="$PWD/build/debug/apps/PhotonStackCLI/photonstack" \
PHOTONSTACK_TEST_SCRATCH_PATH=/private/tmp/PhotonStackAuditSwiftBuild \
  tools/scripts/test-swift.sh --build-system native \
  --filter 'PhotonStackAppCoreTests|PhotonStackProcessingTests|[Rr]eusableWorkflow|[Dd]eepSky|audit'
```

本轮日志：`/tmp/photonstack-followup-fixes-regression.log`。

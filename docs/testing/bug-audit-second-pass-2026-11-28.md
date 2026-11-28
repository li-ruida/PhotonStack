# 第二轮审计核实与修复（2026-11-28）

针对用户提供的 Swarm 复查记录，对当前 `bug-audit` 工作区继续核查。保留上一轮所有未提交修复，本轮没有提交、推送或安装应用。

## 核实结论

报告的 H7 和 14 个编号项均包含实际可修复的缺口，但部分描述需要限定范围：

- **H7 是条件性生命周期缺陷**。原代码把所有非 NSPanel 窗口标为已编辑，关闭后也未清除，而退出检查会包含驻留的已关闭窗口；不是每次关闭必然卡死。
- **目录尾斜杠风险在本机实际复现**。临时目录实验中 `is_symlink(link)` 为 true，`is_symlink(link/)` 为 false；`remove_all(link/)` 删除了目标目录中的标记文件并留下链接。不同命令触发危险清理的前提不同，不能说 13 个命令任意调用都会删除文件。
- **第 14 项不是完全未跟踪的后台任务**：`launch` 已设置 `activeTask`，`hasActiveProcessing` 也包含它；实际缺口是缺少 `runJob` 的统一取消/完成处理，以及保存回调缺少项目身份检查。已补齐这两层保护。
- **M3 仍为误报**：极小 sigma 的中间 NaN 不等于最终输出全图 NaN，本轮不改其输出算法。
- **M10、L4** 仍属于此前记录的产品生命周期/参数组合约定，本轮不更改。

## 按报告编号的处理

| 项目 | 修复 |
| --- | --- |
| H7 | `isDocumentEdited` 改由绑定真实工作区窗口的 `WindowCloseGuard` 同步；关闭或解绑时清除。退出只检查可见或最小化的已编辑非面板窗口。设置与控制台窗口不再被批量标脏。 |
| 1：M2 后续 NaN | 多项式拟合、鲁棒内点选择及基线 RMS 排除不可逆映射/非有限配对，按有效样本数量归一化；线性求解与输出系数/RMS 检查有限性。一个消逝线点不会污染整帧有效配对。 |
| 2：新增校准帧 | `addAssets` 对真正新增的 dark/bias/flat 调用校准列表同步；保留外部母片，更新已有显式空/非空列表，失效旧报告。 |
| 3：曲线工作流 RAW 参数 | `.curves` 可移植参数集合包含全部 `rawKeys`，保存与重放不再剥离解码设置。 |
| 4：目录尾斜杠与链接 | 在共用 CLI 分发入口规范化 `--output-dir` 和校准命令 `--job` 的尾部分隔符/`.`，拒绝终端符号链接（含悬空链接）；普通路径保留可用性，工作流步骤也经过同一入口。 |
| 5：RA 文本舍入 | OpenNGC 轮廓坐标改用 `17g`，避免有效的 `359.9999999999999` 被重新写成 `360`。 |
| 6：GHS 取消打开 | 取消按钮同时取消视图持有的 operation 与 session 内部任务。 |
| 7：背景配方覆盖项目 JSON | 保存前使用工作区统一导出保护，包含项目文件、受保护输入和当前背景源，识别链接别名。 |
| 8：校准 Session 竞态 | 取消只发出取消请求，busy 和 task 由任务实际退出时清理；使用 defer 覆盖早返回。保存完成仅清理自己的 draft，并在回调前检查取消与任务代次。已发布条目保留，取消后不触发关闭弹窗回调。 |
| 9：拼接路径身份 | 面板识别、代表素材匹配、组成员去重和侧边栏折叠统一使用解析链接后的文件路径身份。 |
| 10：重分组位置参数 | `.mosaic` 转 `.light` 通过 `updateRole` 完成，复用参考帧、权重及相关设置同步。 |
| 11：workers 负数回绕 | `--workers` 先要求 ASCII 十进制数字，再解析并检查 1…64，拒绝绕回正数的超大负整数。 |
| 12：clouds 输出声明 | detect/temporal-detect 明确拒绝无意义的 `--output`，不再成功报告一个从未写出的图像文件；实际检测仍通过标准输出返回 JSON。 |
| 13：可选 Python | 所有 Python CLI 测试都放入 `Python3_Interpreter_FOUND` 条件块，无 Python 配置不会注册不可执行的 Python 测试。 |
| 14：派生成片回写 | `showDerivedImage` 使用 `runJob` 并在变更前检查取消与项目身份；PlateSolve/SPCC 保存完成后检查取消及发起时项目 ID，再使用结果。 |

## 验证范围与证据

- CMake Debug 构建通过。完整 CTest **84/84 通过**，包含本机 Metal 测试；原有两条废弃 API 警告仍在。
- 直接覆盖私有拟合边界的 `RegistrationFiniteFitTests.cpp`：64 个有效配对加一个消逝线点、一个 NaN 点，普通和鲁棒拟合均保留有限、准确的结果；全无效配对拒绝。
- CLI 回归覆盖终端链接/悬空链接的多种尾斜杠写法、跨命令入口拒绝与目标内容保持、校准任务尾斜杠读取、合法新目录写出、workers 回绕和云检测输出契约。
- OpenNGC 回归同时覆盖 `-1e-15` 和 `-1e-13` 输入，检查最终文本读回后的 RA 范围。
- 无 Python 配置验证：`CMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE` 下配置成功，剩余 60 个 CTest，无 Python 测试被注册。
- Swift 定向测试 **27 项通过**，包括原生 NSWindow 关闭后清理编辑标记、两类 Session 取消期间锁定、GHS 打开取消、项目 JSON 导出保护、符号链接素材面板导入、派生成片取消、校准素材列表、RAW 参数和重分组参考/权重。
- 完整 Swift 回归 **649 项：632 通过、17 因真实数据/外部服务条件跳过、0 失败**。未跳过整个 WorkspaceModel 测试套。跳过项包括真实 M31、Gaia、外部求解器及专用集成环境；原有 QualityQA Swift 6 隔离警告仍在。
- `git diff --check` 通过。
- 旧 `constructionSessionInvalidatesDraftAndCleansTemporaryProducts` 测试改为等待取消任务真正退出，再检查资源清理；不再把发出取消请求当作完成。

原生窗口测试覆盖编辑标记归属、同步和关闭清理；没有通过已安装 App 手动操作整套“设置窗口—放弃关闭—退出”交互，也没有把合成数据测试当作真实天文图像质量验收。

日志：

- `/tmp/photonstack-round2-build.log`
- `/tmp/photonstack-round2-ctest-focused.log`
- `/tmp/photonstack-round2-ctest-full.log`
- `/tmp/photonstack-round2-swift-focused.log`
- `/tmp/photonstack-round2-swift-full.log`
- `/tmp/photonstack-round2-no-python.log`

主要验证命令：

```sh
cmake --build build/debug -j 4
ctest --test-dir build/debug -j 4 --output-on-failure
PHOTONSTACK_TEST_CLI="$PWD/build/debug/apps/PhotonStackCLI/photonstack" \
PHOTONSTACK_TEST_SCRATCH_PATH=/private/tmp/PhotonStackAuditSwiftBuild \
  tools/scripts/test-swift.sh --build-system native
cmake -S . -B /private/tmp/PhotonStackAuditNoPython -DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE
ctest --test-dir /private/tmp/PhotonStackAuditNoPython --show-only=json-v1
```

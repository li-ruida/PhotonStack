# 校准库：素材检查与匹配规划

当前提供原生 FITS 拍摄信息检查、逐张母片匹配、批量分组与候选规划，以及 App 内的母片保存和匹配审查。**尚未接入校准执行或一键成片**。`compatible` 只表示已声明的拍摄条件相符，不证明像素有效、文件确实是母片、偏置扣除状态正确或校准已执行。匹配和规划报告均明确返回 `scope: acquisition-only`、`canApply: false`、`pixelsValidated: false`。

## App 母片库与匹配审查

工具栏的“校准库”（⌘⌥C）打开母片管理和匹配审查。可导入已有偏置、暗场、平场，记录显示名称、偏置是否已扣除及备注。已知 FITS 类型与所选角色冲突时拒绝导入；这些声明仍不是母片制作或像素验证的证据。

母片复制到 `~/Library/Application Support/PhotonStack/CalibrationMasters`，以完整文件 SHA-256 标识；重复导入相同内容不覆盖既有名称和记录，角色或偏置声明冲突会报错。支持重新核对、归档、恢复及导出到新文件夹。归档保留母片；导出包含母片副本与导入记录。匹配及导出前重新检查文件摘要和拍摄信息。已保存的声明暂不支持编辑。测试可用 `PHOTONSTACK_CALIBRATION_LIBRARY` 指定隔离库。

匹配审查可递归选择 FITS 亮场文件夹，或使用当前工作区的亮场。母片列表的复选框决定本次候选；重新载入保留对现有母片的排除。界面分别显示分组、组内各帧、候选的匹配数量及冲突原因；逐帧结论与整组可共用母片的结论分开显示。没有母片也可检查素材分组。修改候选、素材或温度容差后必须重新检查，旧结果不可导出；容差限 0–10°C。

可导出与原生规划格式一致的 JSON 审查报告。App 当前报告读取上限为 32 MiB，小于 CLI 的 64 MiB；过大批次需拆分。库记录持久保存，本次素材选择和审查会话尚未作为项目持久化。

## 检查输入

```sh
photonstack calibration-inspect /absolute/light-001.fit /absolute/master-dark.fit
photonstack calibration-match \
  --input /absolute/light-001.fit --master /absolute/master-dark.fit \
  --role dark --temperature-tolerance 1
```

角色支持 `bias`、`dark`、`flat`、`dark-flat`；最后一种用于平场的暗场校正。可用 `--expected-input-sha`、`--expected-master-sha` 指定先前记录的 SHA-256，拒绝已变化的文件。检查批次为 1–256 个 FITS；目前只检查支持的主图像 HDU，不接受 RAW、压缩 FITS 或任意扩展表作为传感器校准输入。检查器能描述 RGB 的维度，但匹配器拒绝插值后的多通道图像作为传感器校准数据。

结果保留原文件路径、SHA-256、字节数、尺寸、BITPIX、已知字段、字段来源关键词和异常。没有字段就保持缺失，不从文件名、相机型号或像素分布推断。读取前后分别计算完整文件摘要，绑定这一次文件头检查；这会产生完整文件 I/O，并非仅读取几个头字段。任何无法读取或截断的文件都会使本次命令失败，不输出可误用的半份 JSON。命令不修改原图，也不生成校准图像。

### 字段与语义

| 字段 | FITS 来源 | 约束 |
| --- | --- | --- |
| 相机、实体相机标识 | `INSTRUME`、`CAM-SER` | 型号相同不证明是同一台相机 |
| 曝光 | `EXPTIME` / `EXPOSURE` | 两者同时出现必须一致，单位秒 |
| 相机增益、ISO | `GAIN`、`ISOSPEED` | 零增益是有效数值，缺失不能变成零 |
| 电子转换增益 | `EGAIN` | 电子/ADU，不作为 `GAIN` 的替代值 |
| 实测、设定温度 | `CCD-TEMP`、`SET-TEMP` | 使用实测温度匹配；设定温度不是测量值 |
| 温度单位 | `TEMPUNIT` | 支持 C、CELSIUS、DEGC；无该标签时按 CCD-TEMP 的摄氏温度约定；明确其他单位会阻止匹配 |
| binning 与传感器区域 | `XBINNING/YBINNING`、`XORGSUBF/YORGSUBF` | 不把缺失的 binning 当作 1，不把缺失的区域起点当作 0 |
| 拜耳相位 | `BAYERPAT`、`XBAYROFF/YBAYROFF` | 比较偏移后的有效排列；相位偏移缺省为 0 |
| 显式单色布局 | `PSLAYOUT=MONO` | 没有 BAYERPAT 不足以证明是单色传感器 |
| 单位、线性域 | `BUNIT`、`PSCOLOR` | 保留未知状态；非线性域不能用于线性校准 |
| 滤镜、光路标识、焦距 | `FILTER`、`PSOPTICS`、`FOCALLEN` | 平场要求滤镜和光路一致；PSOPTICS 必须随旋转等光路改变而更新 |
| 读出模式、偏置 | `READMODE`、`OFFSET` | 缺失与明确相等分开报告 |
| 目标、时间 | `OBJECT`、`DATE-OBS`、`TIMESYS` | 不使用文件修改时间或目录名称冒充拍摄信息 |

`PSLAYOUT`、`PSOPTICS` 和 `PSCOLOR` 是应用约定，不应假设其他采集软件会写入。`CAM-SER` 等设备标签也可能缺失。暂未提供人工补充元数据的侧车文件；不能为了消除“待核对”而改写原片头。

FITS 数字支持 D 指数。字符串正确处理引号转义与字符串内斜线。重复关键字段、矛盾别名、NaN、无效范围、未闭合字符串、引号内数值会被标记。FITS 存储缩放的缺省值 `BSCALE=1`、`BZERO=0` 是格式规定，与缺失拍摄参数不同。

### 匹配结果

- `compatible`：要求的拍摄字段均已知且相符。
- `review`：没有明确冲突，但缺少必要信息，不能自动视为匹配。
- `incompatible`：至少一个明确冲突或非法字段。

暗场匹配曝光、实测温度、相机、读出、增益/ISO、偏置、binning、尺寸、区域和有效拜耳相位。暗场和偏置不要求与亮场使用相同滤镜；平场要求滤镜及光路一致，但不要求与亮场等曝光、等温度。平场本身的暗场校正属于单独步骤。默认温度容差是显式可调的 1°C，允许 0–10°C；曝光差不超过 1 微秒。不隐式缩放暗场。

## 批量分组与候选规划

清单使用 UTF-8，每行记录角色、双引号包围的绝对路径和预期 SHA-256。`-` 表示本次读取时获取摘要；可用报告中的摘要固定下一次检查。路径中的引号和反斜线用反斜线转义。

```text
PhotonStack-CalibrationPlan-1
temperature-tolerance 1
light "/absolute/night-1/Ha/light-001.fit" -
light "/absolute/night-2/OIII/light-002.fit" -
dark "/absolute/library/dark-60s.fit" -
flat "/absolute/library/flat-Ha.fit" -
```

```sh
photonstack calibration-plan --manifest /absolute/acquisition-list.txt
```

亮场规划清单支持 `light`、`bias`、`dark`、`flat`；平场制作及其暗场另行处理。清单最大 1 MiB，最多 4096 张亮场、256 张候选母片、262144 个亮场×母片组合。报告超过 64 MiB 时要求拆分批次或减少候选，避免大量异常组合耗尽内存。重复路径、相同文件内容重复计入、摘要不符、非法角色或与亮场角色矛盾的文件头会使规划失败。

按目标、UTC 日期、滤镜、相机、读出、增益/ISO、曝光、偏置、binning、区域、采样布局和已声明光路等组织素材。尺寸与通道数也进入分组。缺失目标或无效日期的图像各自保留，不静默并入同一未知组。UTC 日历日期**不是观测地的整夜标签**；跨午夜的本地观测夜管理还需上层工作流处理。

`DATE-OBS` 按 FITS 的日期语法检查，缺省 `TIMESYS=UTC`。当前只按 1972 年起的 UTC 日期分组；其他时标、非法日期或时区后缀不会被当作 UTC。日期可省略时分秒；有时间部分时要求完整时间及可选小数秒。这一规则依据 [FITS 4.0 的时间关键字定义](https://fits.gsfc.nasa.gov/standard40/fits_standard40aa-le.pdf)，不使用本机时区修正未声明的采集时间。

组内实测温度可以变化，匹配会逐张进行。报告包含：

- `lights` / `masters`：本次读取的完整拍摄信息快照。
- `groups`：成员索引、分组字段、分组异常，每个候选母片的匹配/待核对/冲突数量，以及异常字段、首个示例成员和原因。
- `frameChoices`：每张亮场的组索引、各角色的可匹配和待核对母片索引。一个暗场不能覆盖整组时，仍保留各帧的不同候选，供后续分配校准组。
- 各角色决策：`unique-compatible`、`ambiguous`、`review-required`、`no-compatible-master`、`missing-master`。多个兼容母片不会按文件名或目录顺序随意选一个。`suggestedMaster` 仅在恰好一个兼容候选时给出，仍不代表允许应用。

输入清单顺序不影响排序、分组或候选决策（清单自身摘要会随文本变化）。组级决策与逐帧决策分别呈现：可给整组共用的母片，与只适用于部分成员的母片不是同一结论。

## 验证与后续

`tests/cli/test_calibration_metadata.py` 使用手写 FITS 验证两夜×两滤镜×两曝光、逐帧温度变化、多个候选歧义、不匹配平场、缺失参数、错误类型、文件摘要、日期、输入只读等。`tools/scripts/qa/calibration_metadata_validation.py` 用独立 Astropy 读取真实文件头、核对每个已知字段和原始摘要，并测量包括完整哈希读取在内的 CLI 时间。

尚待完成：人工补充拍摄元数据、可审计的声明编辑、同组内校准分配、传感器域母片制作与偏置状态验证、按组执行及断点恢复、审查项目持久化。现有 App 手工校准执行尚未使用这里的新检查；不能用文件头匹配或界面测试替代校准像素与实拍成片验证。

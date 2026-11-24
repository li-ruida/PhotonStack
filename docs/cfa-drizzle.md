# CFA Drizzle

深空堆栈的处理参数支持 `CFA Drizzle（原始传感器采样）`。默认关闭。先用解码后的图像测量星点、筛片和配准，再用原始 CFA 单通道样本及原生尺度的配准变换累积 RGB；去马赛克和配准插值的结果不会成为累积输入。累积阶段目前使用 CPU。

## 使用条件

- 输入为显式 Bayer FITS，可为原始拍摄或校准管理生成的线性 CFA 亮场。支持四种 Bayer 排列及存储坐标中的奇偶偏移。
- 先在校准管理完成暗偏平、坏像素处理。Drizzle 配方不接受内联校准母片，避免两条校准链混用；已校准 CFA 中的负值和 NaN 保持。
- 当前采用平均，可设置逐帧正权重；不提供异常像素剔除、逐帧背景归一化、固定纹理模型、独立亮度噪声估计或畸变配准。支持无配准、平移、相似和仿射变换。
- 输出倍率 1–3，像素足迹比例 0.1–1。越小的足迹越依赖密集的亚像素位移，可能留下缺色；增大尺寸不等于提高真实分辨率。
- RGB 分别按有效覆盖权重归一化，保持表面亮度。以输出像素积分测光时须考虑像素面积 `1 / scale²`，不能直接比较不同倍率的像素总和。
- 缺失颜色保持 NaN，只有三个颜色都覆盖时 alpha 才为 1。没有任何完整 RGB 像素时返回错误，保留诊断母片与权重；不会填洞冒充观测。
- 输出保留完整参考画布。显影可按完整覆盖裁边；关闭裁边且存在缺色时，要求完整覆盖的降噪操作会拒绝执行。背景保护区等坐标参数按输出母片像素填写，改变倍率后需要复查。

设置开关会切换为相容的平均合成，保留后期参数。选择其他合成方式或畸变配准会关闭 Drizzle。CLI 导入配方始终严格校验，不静默改写参数。

## CLI 配方

在 `PHOTONSTACK_DEEP_SKY_RECIPE 1` 配方中设置：

```text
stack.cfa-drizzle "on"
stack.drizzle-scale "2"
stack.drizzle-pixfrac "0.8"
stack.method "average"
stack.median-mad "off"
stack.rejection-maps "off"
stack.normalize-background "off"
decode.debayer "on"
calibration.sensor-pattern "off"
registration.mode "similarity"
denoise.noise-model "scene"
```

保留常规输入、选片、配准和后期设置。运行 `photonstack deep-sky --recipe recipe.txt --output-dir 新目录`，先用该命令的预检能力检查来源和存储估算。App 的同一参数页和流程调用这条引擎路径。

## 产物与身份

- `stack-linear.fits`：Float32 RGB 与完整颜色覆盖 alpha；负值不截断。
- `drizzle-weights.fits`：RGB 各自的覆盖权重，零权重是有效诊断值，因此该文件 alpha 恒为 1。
- `drizzle-inputs.txt`：源文件 SHA、有效 Bayer 排列、权重、配准变换、尺度、足迹和母片/权重摘要。
- `report.json`：逐色覆盖率、完整 RGB 覆盖率及诊断路径。

存在完整支持的参考帧 TAN/TAN-SIP WCS 时，母片和权重的 CRPIX、CD 与 SIP 系数随倍率变换；不完整或不支持的坐标解报错。缺失 WCS 不猜测。来源在累积前后复核 SHA，App 重新后期处理保留原母片和覆盖诊断。预计内存超出工作预算时要求减小倍率。

## 本地验证范围（2026-11-20）

公共 CLI 测试覆盖四色相位的独立像素真值、原始/已校准输入、负值、缩放 WCS、逐色权重、整数 BLANK、NaN 缺色和非法组合。Swift 集成测试覆盖设置联动、完整工作流与重新后期处理时保留诊断。

合成欠采样星场：64 张、8×8 位移相位、物理像素积分的 Gaussian 星点，sigma 0.65–0.85 原始像素，2 倍 Drizzle，pixfrac 0.5。相对独立的半像素积分真值，亮区 RMS 为 9.994 ADU；普通双线性解码/堆栈再双线性放大为 42.746 ADU。配准平移误差中位 0.0192 原始像素。此结果只证明该受控条件下的重建收益。复现脚本：`tools/scripts/qa/cfa_drizzle_resolution.py`，需 NumPy、SciPy、Astropy 和新输出目录。

M31 实拍 20 张同夜 30 s：使用相同选片和配准，1.5 倍、pixfrac 0.8。完整 RGB 覆盖 99.999973%；2772 个未饱和候选星的孔径通量比中位 0.99508，P10–P90 为 0.9471–1.0527；星点矩宽比中位 1.0875，质心距离 P95 为 0.1445 原始像素。**该实拍未证明分辨率提升，星点矩宽反而增大；不能把覆盖充分或放大尺寸当作清晰度验收通过。** 原片哈希全部保持不变。

本机此子集普通平均耗时 13.62 s，Drizzle 46.56 s，峰值 RSS 分别 2.16/2.60 GB。OS 缓存未控制，其他应用可能活动；这不是性能保证。实拍记录保留于 `/tmp/photonstack-cfa-real/validation.json`，受控场景记录于 `/tmp/photonstack-cfa-resolution/validation.json`。

后续去掉整组对齐图临时缓存，在同组素材上复测为 45.36 s，峰值对齐工作文件从 2,654,265,600 bytes 降为 0；四个主要 FITS/TIFF 产物 SHA 与优化前完全相同。记录：`/tmp/photonstack-cfa-real/no-cache-validation.json`。

隔离 App 实际从导入 64 张合成 CFA、启用开关与修改 pixfrac、分析、确认堆栈、显示逐色覆盖到打开后期成片均已执行，截图与可访问性记录：`/tmp/photonstack-next-qa/cfa-settings.png`、`cfa-coverage.png`。

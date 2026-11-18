# GHS 广义双曲拉伸

后期调整 → 高级处理 → 色彩与显示修整 → **GHS 拉伸与预览**。可以从当前图像开始，也可以在面板内另选线性 FITS 母片或已显影的 TIFF/PNG/JPEG。拉伸前后共用输入黑白场，显示全图亮度直方图和本次曲线；参数变化后需更新预览才能另存。打开保存的结果目录可恢复原始来源、输入范围、曲线参数及预览。

## 参数与颜色

- 强度为 `ln(1+D)`，范围 0–10；0 是恒等曲线。
- 局部强度 `b` 为 −5–15。正值采用双曲型、0 为指数型、负值采用积分型；−1 为对数极限。数值实现对 0、−1 附近使用稳定表达式。
- 中心 `SP`、暗部保护 `LP`、亮部保护 `HP` 满足 `0 ≤ LP ≤ SP ≤ HP ≤ 1`。在 `[0, LP]` 和 `[HP, 1]` 使用与核心曲线相切的直线。这保留端部对比变化的平滑性，并不保持该区间原亮度不变。
- 输入范围以原文件数值为单位，RGB 共用。自动范围在线性图中取所有有效颜色样本的最小/最大值，在显示图中取 0–1；首次预览后固定，可以手动修改。无动态范围的线性图需明确指定范围。不使用隐藏的百分位截断。报告分别计数低于黑场、高于白场的有效像素，一个像素任意颜色超出即计数。
- **按亮度缩放**采用 Rec.709 权重 0.2126 / 0.7152 / 0.0722。越界时与逐通道拉伸结果混合，选用最大的共同系数，使三个输出通道都不超过 1；报告混合像素数。未触及色域限制时保持通道比例。**RGB 独立**分别应用同一曲线，可能改变色彩比例。

解析曲线及保护定义依据 [GHS 作者说明](https://www.ghsastro.co.uk/doc/scripts/GeneralisedHyperbolicStretch/GeneralisedHyperbolicStretch.html)，亮度缩放的越界混合依据 [Siril 官方拉伸说明](https://siril.readthedocs.io/en/stable/processing/stretching.html)。独立实现数学公式，没有嵌入外部处理程序或复制其源代码。

## 文件与重跑

```sh
photonstack ghs-stretch --input linear.fits --output-dir new-result \
  --log-stretch 5 --local-intensity 3 --symmetry 0.012 \
  --shadow-protection 0.002 --highlight-protection 0.85 --mode luminance

photonstack ghs-inspect --recipe new-result/recipe.txt
photonstack ghs-replay --recipe new-result/recipe.txt --output-dir replay-result
```

需要固定输入范围时同时传入 `--black` 与 `--white`。`--preview-width 1200` 仅保存预览和配方，宽度上限 2048；0 或省略表示完整输出。预览先在原始分辨率计算拉伸，再缩小显示图，统计始终来自全图。预览配方重跑也保持预览模式；App 从该配方另存时会重新计算全尺寸输出。

完整目录包含：

| 文件 | 内容 |
|---|---|
| `developed.tiff` | 全尺寸 16 位 sRGB 显示成片；App 采用此文件继续普通后期 |
| `display.fits` | 全尺寸 Float32 非线性显示值与覆盖率，`PSCOLOR='SRGB-DISPLAY'`，保留有效 TAN/SIP 坐标 |
| `before.tiff`、`after.tiff` | 相同尺寸的 16 位对比预览；大于 1200 px 宽时缩小 |
| `report.json` | 来源、范围、参数、覆盖/截断/色域计数、前后直方图、1025 个曲线采样 |
| `recipe.txt` | 来源 SHA-256、参数和全部产品 SHA-256 |

来源始终只读；已有输出目录不能替换。预览、另存和重跑检查来源摘要，重开还检查保存产品。FITS 显示标记是 PhotonStack 约定，不是通用 FITS 拉伸标准。PhotonStack 的线性母片入口会拒绝该标记，普通 FITS 预览也不会对它再次自动拉伸。外部软件可能不识别此自定义标记，因此科学处理和测光仍需原始线性母片。

## 验证与边界

- 原生测试检查全部曲线分支、参数边界、0 和 −1 邻域、单调性、端部一阶连续性、恒等变换、已知调和/对数曲线、颜色越界、掩膜、部分覆盖及显示 FITS 数值往返。
- CLI/Swift 集成覆盖负信号范围、NaN 掩膜、WCS、源文件不变、预览/全图同统计、重跑一致、打开恢复、参数变化后的旧预览禁止保存、非法范围、CFA 拒绝、来源/产品改变和已有目录保护。
- [独立全图复验](../tools/scripts/qa/generalized_stretch_validation.py) 使用 NumPy/Astropy/tifffile 检查所有像素、直方图、覆盖、截断与色域统计、16 位量化及重跑；[Siril 与分支复验](../tools/scripts/qa/generalized_stretch_siril.py) 另检查灰阶和独立数学公式。需要自行提供 Siril 及隔离配置；不改用户配置。
- 当前为 CPU 双精度解析计算，最大 8 个工作线程；不是 GPU 实现。预览需要完整解码、计算和读取摘要，采用明确的“更新预览”，滑块拖动期间不反复启动处理。
- 当前没有 GHS 逆变换、饱和度域拉伸、单独选择 RGB 通道、直方图放大/取样或多步配方链。曲线提升暗部也会提升噪声；明亮或锐利的显示效果不代表信噪比、测光或分辨率提高。多尺度自适应拉伸、进一步降噪与完整能力清单仍继续开发。

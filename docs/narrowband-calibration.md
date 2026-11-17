# 双窄带原片校准与可恢复配方

入口：后期调整 → 高级 → 双窄带与 HOO → 原片提取。开启“提取前校准传感器”，选择偏置、暗场、平场母片。单张使用“提取并保存两张 FITS”；同一目标的至少三张亮场可使用“选择多张原片并堆栈”。结果自动进入主画布，可查看 Hα、OⅢ、HOO、有效性与采用权重。

## 输入要求

- 亮场和母片均为未去马赛克的单平面线性 FITS，明确写有 `BAYERPAT`，尺寸与有效拜耳排列一致。支持四种标准拜耳排列、相位偏移、BSCALE/BZERO 和负信号；RGB 母片、已经提取的半尺寸通带图不能用于传感器校准。
- 用户需确认相机、增益、binning、温度、光路、滤镜与拍摄区域相符。程序比较尺寸、有效拜耳排列，以及**双方提供时**的 INSTRUME、GAIN、XBINNING、YBINNING；使用暗场时还比较曝光时间（EXPOSURE 或 EXPTIME）。缺少元数据不会被当成匹配证明，温度与光学配置暂不自动匹配。
- 暗场需与亮场同曝光，当前不做暗场缩放或暗电流优化。平场需完成其自身所需的暗电流处理；此入口只能根据明确状态再减去偏置，没有暗平场合成入口。
- 本入口接收已经生成的母片，不替代校准库的分组、质量筛选、母片构建和跨夜匹配。

## 偏置处理

| 母片状态 | 处理方式 |
|---|---|
| 暗场仍含偏置 | 亮场只减暗场，不再重复减偏置 |
| 暗场已去偏置 | 需要偏置母片；亮场分别减暗场和偏置 |
| 平场仍含偏置 | 需要偏置母片；先从平场减偏置再归一化 |
| 平场已去偏置 | 直接归一化，不再减偏置 |
| 仅提供偏置 | 从亮场减偏置 |

暗场、平场的偏置状态必须明确选择，配方记录原始选择，不从图像亮度猜测。平场以 R、G1、G2、B 四个拜耳位置的有效正样本均值分别归一化。对每个传感器像素先作减法、再除归一化平场，最后提取通带；不会先把 G/B 合成 OⅢ 再套用平场。

保留负值。母片 NaN、非正平场或小于其相位均值百万分之一的平场样本变为无效像素，而非零亮度信号。单张提取分别保留 Hα 与 OⅢ 的有效性；多帧堆栈目前仍只使用两条通带都有效的完整像素格。

校准先建立只读的逐像素扣除量和倍率，多张亮场复用；没有逐帧重复加载、计算平场均值。此路径为 CPU 计算，没有据此宣称 GPU 加速。

## 命令行

```sh
photonstack narrowband-extract --input light.fit --output-dir extracted \
  --dark dark.fits --dark-bias included --flat flat.fits --flat-bias removed

photonstack narrowband-stack --output-dir stacked \
  --bias bias.fits --dark dark-without-bias.fits --dark-bias removed \
  --flat flat-without-bias.fits --flat-bias removed \
  light1.fit light2.fit light3.fit

photonstack narrowband-inspect --recipe stacked/recipe.txt
photonstack narrowband-replay --recipe stacked/recipe.txt --output-dir repeated
```

所有输出目录必须是新目录。版本 2 配方保存亮场和校准母片的完整路径、SHA-256、母片角色、偏置状态及产品摘要，重开、重跑都验证来源；原来的版本 1 无校准配方仍可读取。文件被修改会报告错误，取消或失败不会替换已有母片。重配 HOO 以校准后的两张线性母片为输入，不重复应用传感器校准；其来源提取/堆栈配方应一并保留。

## 验证范围

数值、App 操作、实拍副本注入实验与 Siril 对照见[验收记录](advanced-imaging-validation.zh-CN.md)。校准操作和通带提取的算法须分别验收：正确扣除偏置与渐晕，不代表 OⅢ 的采样、插值和星点质心已经与其他软件等价。

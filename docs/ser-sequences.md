# SER 序列导入

SER v3 的单色、四种 RGB Bayer 排列和交错 RGB/BGR 可以逐帧解码到独立 Float32 FITS。保留原始像素码值、Bayer 相位、已提供的相机/望远镜信息及逐帧 UTC 时间；不做去马赛克、颜色平衡、拉伸或重采样。原始 SER 保持只读。

```
photonstack ser-inspect --input capture.ser
photonstack ser-extract --input capture.ser --output-dir new-sequence \
  --first 0 --count 200 --step 1 --byte-order compatibility
```

帧索引从 0 开始，`count` 是抽取数量，`step` 是帧间隔。每次最多 10000 帧、单帧最多 32 MP；逐帧处理，写出前检查磁盘空间。输出目录必须不存在，整批通过后才发布。`sequence.json` 记录来源及每张 FITS 的 SHA-256、原始索引、原始时间刻度和实际字节序。

字节序默认采用 Siril / SER Player 的通用约定（标志 0 为 little endian）。`specification` 采用原始规范相反的解释，也可强制 `little` 或 `big`。这是已知的生态兼容差异，不能仅由像素自动推断。参见 [Siril 官方 SER 文档](https://siril.readthedocs.io/en/latest/file-formats/SER.html) 与 [SER v3 原始规范](https://grischa-hahn.hier-im-netz.de/astro/ser/SER%20Doc%20V3b.pdf)。

1–7 位样本按规范的 MSB 对齐解码为原始位深码值；9–16 位按 LSB 对齐。与位深冲突的样本、截断帧或时间表、未知 Bayer 编码均拒绝，不能静默裁剪。CMYG 暂不支持。

时间表保存 100 ns 整数刻度（JSON 使用字符串避免精度丢失），有有效逐帧时间才写 FITS `DATE-OBS`。只提供序列开始时间时，不臆造每帧时间或曝光时长。零时间记录为缺失，选中序列的时间不递增会在报告中指出。曝光/增益/响应曲线不是 SER 的可靠字段，后续科学校准需要独立输入。

当前已通过独立字节夹具对照：大小端、规范覆盖选项、1/5/8/12/16 位、四种 CFA、RGB/BGR、日期、步进、源文件不变、错误中途清理和已有目标保护。App 入口与实际 SER 软件互操作仍在验证。

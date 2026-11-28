# GPU、RAW 与高斯路径复核

本轮在已有优化上继续验证。大图曲线新增 Metal LUT 与输入校验融合；高斯处理保留 FP64 及原来的逐像素 tap 顺序，改为让独立像素连续运算。没有将科学模糊替换为 FP32、IIR 或其他近似。

## 测量

以下是本机 Release 合成夹具测量，不代表真实天文数据的画质验收，也不是所有尺寸和机器的固定加速比。

| 操作 | 基线 | 当前 | 结果校验 |
| --- | ---: | ---: | --- |
| 2.1 MP RGB/单通道曲线，预热后 8 种模式中位数 | CPU 3.05 ms | GPU 1.78 ms | 全像素 `memcmp` 相等 |
| 24 MP RGB 曲线，后两轮中位数 | CPU 39.69 ms | GPU 12.86 ms | 全像素 `memcmp` 相等 |
| 局部对比度，半径 32 | 31.29 ms | 18.54 ms | 输出文件逐字节相同 |
| 局部对比度，半径 64 | 52.17 ms | 33.78 ms | 输出文件逐字节相同 |
| 科学重建保护，sigma 16 | 24.89 ms | 18.41 ms | 输出文件逐字节相同 |
| 科学重建保护，sigma 64 | 128.95 ms | 112.99 ms | 输出文件逐字节相同 |

高斯夹具为 769×513 的带符号 HDR RGBA，含零覆盖、部分覆盖和遮蔽 NaN；基线使用本轮开始时保存的引擎库，当前库还包含本轮线程池和 IPO 改动，因此表中数字是合并后的收益，不是对单一代码变更的隔离归因。每项跑三轮，表中采用后两轮中位数。

GPU 曲线初版仍在 CPU 串行校验全图，2 MP 测量反而略慢，未启用该版本。最终版本把校验与 LUT 映射融合后才接入曲线预览。24 MP 首次 GPU 调用为 139 ms，包含进程内一次性的 Metal 库/管线初始化；预热后的数据不能用于描述冷启动延迟。

测量原始记录在本地 `artifacts/performance-20261128-followup/`：`gaussian-before.csv`、`gaussian-after.csv`、相应 `.f32` 输出目录、`curves-24mp-fused.csv` 及 `metal-fused-validation.txt`。最后一个文件在曲线用例之后还记录了一个不受支持的 TIFF+线性 FITS 暗场测试失败；最终回归改为校准 FITS，最终整体测试结果见本轮集成报告。

## 最终实现边界

- 曲线桥接请求启用 Metal；实际门槛是至少 1 MP、RGBA、RGB 或单一 RGB 通道。小预览、灰度、FP64 保色亮度曲线继续使用 CPU。
- GPU 同时检查覆盖规则、有限值及结果；不支持的精度边界（例如极小数）回退 CPU，保留原有错误优先级。测试包含映射/保留 alpha、遮蔽 NaN、可见 NaN、非有限 alpha 和 denormal 回退。
- 曲线采用现有 CPU 生成的 4097 项 LUT，没有改变曲线拟合方法。输出页面直接成为 `ImageBuffer` 的像素存储。
- `LocalContrast` 用 64 像素小块复用权重、覆盖及 FP64 累加器；每个像素的 tap 顺序不变。普通路径的有效覆盖只在当前 worker 的一行内缓存，没有额外整图覆盖平面。
- `ReconstructionProtection` 的内部水平区域按 tap 遍历连续像素，边界仍使用原半采样反射；没有跨 SIMD lane 重排浮点归约。
- RAW 的 `CIContext` 已按 GPU/软件模式共享，滤镜与输出缓冲仍逐调用独立。本轮把剩余串行有限值校验/夹取改为共享线程池范围任务，用最早非法样本索引保留原错误信息选择。没有可用真实 RAW 夹具时不宣称真实相机解码的数值或吞吐验收。

## 页对齐和异步建议复核

`PixelStorage` 已使用运行时 `getpagesize()` 分配并补齐页面，不能继续按“普通 vector 总是不对齐”判断当前实现。真实 Metal 回归使用非页面整数尺寸，检查地址对齐并执行校准、变形和归约。统一内存路径借用输入页面，并直接交换输出页面；非统一内存/不可借用的输入保留复制回退。

`execute()` 仍为同步契约，调用结束后借用页面才可以释放、输出才可以由 CPU 使用。现有 `orderedAnalysisFrames` 并行运行多个帧任务，`saveAligned` 位于工作任务内；一个 worker 等待 GPU 时，其他 worker 可以读帧、匹配或写已完成帧。两个 Metal slot 保留并发提交所需的独立缓冲。这是从调用链确认的并发能力，本轮没有采集逐帧时间线，因此不声称某个 CPU/GPU 重叠百分比。

仅把 `waitUntilCompleted` 改为 completion handler 再立即等待信号量不会改善这个契约的延迟。真正的异步 API 需要同时改变输出所有权、取消和帧任务提交方式；本轮没有实施这种缺少测量依据的大范围重构。

## 复现工具

- `tools/scripts/qa/gaussian_performance_probe.cpp`：同一源码分别链接基线和当前库，记录耗时并导出原始像素供逐字节比较。
- `tools/scripts/qa/curves_backend_probe.cpp`：最终生产曲线路径 CPU/GPU 交替运行、逐字节核对；无参数为 2.1 MP，任意参数为 24 MP。返回 77 表示 GPU 没有实际执行。
- `tools/scripts/qa/gpu_curves_probe.mm`：独立的 kernel-only 实验，不包含生产输入校验，不能用它的数字代替生产路径测量。

示例（在仓库根目录，先构建 Release 引擎）：

```sh
clang++ -std=c++20 -O3 -flto=thin -Iengine/include tools/scripts/qa/curves_backend_probe.cpp build/release/engine/libphotonstack_engine.a -framework Accelerate -framework CoreFoundation -framework CoreGraphics -framework CoreImage -framework Foundation -framework ImageIO -framework Metal -o /tmp/photonstack-curves-probe
/tmp/photonstack-curves-probe large
```

Metal 测量需要可访问真实 GPU 的执行环境；沙箱中没有设备时的 CPU 回退或 skip 不能算 GPU 验证通过。

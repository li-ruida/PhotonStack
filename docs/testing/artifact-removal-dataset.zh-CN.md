# PhotonStack 卫星/伪影去除测试数据集

> 自动生成于 2026-08-05。仓库内版本与桌面副本由同一脚本维护。

## 当前结论

- 严格逐图数量边界网络集共 50 张；当前代码通过 50/50。
- v22 大规模弱标签集共 500 张；首轮固定运行 300 张，成功处理 300/300，运行错误 0。
- v22 与 Commons 分类弱标签一致 144/300；分类会混入书籍扫描、城市夜景、合成图或同时含多类轨迹，因此该数字只用于人工审核排队，不是准确率。
- AI 已逐图复核全部 500 张：349 张达到高置信可评测标准，151 张进入人工复核/剔除队列。修改前基线为 172/349，当前为 224/349（64.2%）。AI 标签仍是首轮筛选，不冒充人工真值。
- v16–v21 六轮真正首次盲测通过 28/38（73.7%）。这个数字用于监控泛化，不能被修复后的回归成绩替代。
- 桌面逐图覆盖 18 张，运行错误 0 个；桌面图片目前只有检测数量，没有人工像素标注，因此不计入通过率。
- 最终验收使用 `/Applications/PhotonStack.app` 内的 CLI 和真实 macOS 图像解码环境；安装包、打包产物与测试构建的 CLI 哈希一致。
- `DSC_6695.NEF` 已用最终安装版执行实际去除：检测 2 条、移除 2 条；其中底边候选长约 531 像素，放大检查确认用户指出的横线已消失。
- 当前数据只检查每张图的伪影数量上下界，尚不能证明每条轨迹像素都被完整覆盖，也不能证明所有额外候选都正确。

## 首次盲测历史

| 轮次 | 首次结果 | 首次失败样本 | 当前回归 |
| --- | ---: | --- | ---: |
| v16 | 4/6 | `v16-positive-iss-trail`、`v16-negative-perseid-joshua-tree` | 6/6 |
| v17 | 4/6 | `v17-negative-leonid-composite`、`v17-negative-phone-startrails` | 6/6 |
| v18 | 3/6 | `v18-positive-milky-way-satellite`、`v18-negative-leonid-wipf`、`v18-negative-star-trail-night-sky` | 6/6 |
| v19 | 5/6 | `v19-negative-paranal-startrails` | 6/6 |
| v20 | 5/6 | `v20-negative-perseid-2021` | 6/6 |
| v21 | 7/8 | `v21-positive-gaia-dotted` | 8/8 |

v21 的过程特意保留：首次漏掉 Gaia 的中性灰周期点链；加入新检测后，Kitt Peak 彩色地平线与 v18 过曝星轨又暴露误报。最终边界同时约束周期性、色差、暖色偏移和饱和度，并由合成正反例锁定。

## v22：500 张大规模弱标签集

该集合在首次运行前冻结：300 张为冻结评估部分，200 张原为保留开发部分；本轮按用户要求已将后 200 张全部纳入检测和 AI 复核。每项记录 Commons 来源页、许可证、作者/署名、原始尺寸、固定 1280px 下载地址、Commons SHA-1 和下载字节 SHA-256；图像字节只保存在忽略的构建缓存。

| Commons 来源分类 | 清单数量 |
| --- | ---: |
| `Aircraft light trails` | 35 |
| `Circumpolar stars in star trails` | 168 |
| `Meteor showers` | 44 |
| `Meteor trails` | 49 |
| `Meteors` | 57 |
| `Satellite flares` | 17 |
| `Satellite trails` | 48 |
| `Star trails` | 82 |

| 首轮类别 | 尝试 | 成功处理 | 弱标签一致 | 弱标签不一致 |
| --- | ---: | ---: | ---: | ---: |
| `protected-meteor` | 90 | 90 | 24 | 66 |
| `protected-star-trail` | 150 | 150 | 72 | 78 |
| `removable-artifact` | 60 | 60 | 48 | 12 |

这里的“成功处理”表示最终安装版完成解码和检测并返回合法报告；“弱标签一致”只表示检测数量符合 Commons 分类推导出的粗略预期。只有经过人工逐图审核并写入专用边界或掩码的图片，才可升级为严格回归样本。

首轮人工抽查同时看到了真实漏检与分类噪声：`Spacecraft (4934491199)`、`Mars and Milky Way` 等清晰轨迹确有漏检；另一方面，流星分类里混有书籍扫描和城市夜景，星轨图也可能同时包含卫星。为避免用错标数据反向破坏流星保护，本轮先保留原始报告和人工审核队列，不直接按 144/300 调阈值。

## v22 AI 逐图复核

AI 使用 1280px 图片生成的逐图联系表检查全部 500 张图片，并标记为应删除、应保护、混合、无轨迹、不适合或模糊。图表、插画、明显合成图、无关风景和低置信判断不会进入高置信评测清单。该复核能清理 Commons 类别噪声，但仍不是人工像素级真值。

| AI 判断 | 高置信样本 | 修改前基线 | 当前通过 |
| --- | ---: | ---: | ---: |
| `mixed` | 15 | 7 | 6 |
| `none` | 7 | 2 | 2 |
| `protected` | 274 | 123 | 175 |
| `removable` | 53 | 40 | 41 |

高置信清单合计 349 张，修改前通过 172 张，当前通过 224 张、失败 125 张。当前应删除或混合场景通过 47/68；应保护或无轨迹场景通过 177/281。保护类改善明显，但剩余失败说明流星/星轨误报和复杂场景漏检都还没有解决。全部 500 张现已被查看，后续只能作为回归/开发集，不能再称为未见盲测。

机器可读文件按 evaluation/development 两部分保存：`corpus-v22-ai-review*.json`（全部 500 张）、`corpus-v22-ai-high-confidence*.json`（349 张）、`corpus-v22-ai-manual-queue*.json`（151 张），以及修改前和当前的汇总报告。

## 网络样本与当前结果

图片字节不进入 Git。脚本只把固定尺寸版本下载到忽略的 `build/artifact-blindset/`，并用清单中的 SHA-256 校验。作者和许可证以来源页为准。

| 集合 | 样本 | 期望 | 当前检测 | 结果 | 来源 |
| --- | --- | ---: | ---: | --- | --- |
| 基础集 | `dev-geminid-bolide` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Geminid_Meteor_-_Dec._2012.jpg) |
| 基础集 | `dev-meteor-2014` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:2014_Meteor.jpg) |
| 基础集 | `dev-startrails-krushevo` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Star_trail_above_Krushevo.jpg) |
| 基础集 | `dev-startrails-starstax` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Star_trails_(starstax).jpg) |
| 基础集 | `dev-bluewalker-mcmath` | 5–8 条 | 5 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Trail_left_by_BlueWalker_3_over_McMath-Pierce_Solar_Telescope_(ann22033f).jpg) |
| 基础集 | `dev-neowise-iss-starlink` | 3–6 条 | 5 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:2020_F3_NEOWISE_Comet_%26_ISS.jpg) |
| 基础集 | `eval-geminid-church` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Geminid_meteor_over_old_church.jpg) |
| 基础集 | `eval-perseid-2011` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:2011-08-15_23-26-53-meteor.jpg) |
| 基础集 | `eval-startrail-hebei` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Star_trail_in_Hebei.jpg) |
| 基础集 | `eval-startrails-southern` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Southern_hemisphere_star_trails_(7562064954).jpg) |
| 基础集 | `eval-starlink-overpass-2` | 至少 1 条 | 1 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Starlink_Overpass_No_2_(47943840748).jpg) |
| 基础集 | `eval-tianhe-css` | 至少 1 条 | 1 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Tianhe_CSS_as_seen_from_M.B_Gonnet_02.jpg) |
| v16 | `v16-positive-iss-trail` | 至少 1 条 | 1 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:ISS_trail.jpg) |
| v16 | `v16-positive-milky-way-prevalla` | 至少 1 条 | 2 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Milky_Way_Prevalla.jpg) |
| v16 | `v16-negative-perseid-meteor` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Perseid_Meteor.jpg) |
| v16 | `v16-negative-perseid-joshua-tree` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Perseid_Meteor_Shower_above_a_Joshua_tree_(53434073148).jpg) |
| v16 | `v16-negative-circumpolar-star-trails` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Circumpolar_star_trails.jpg) |
| v16 | `v16-negative-hoodoo-startrail` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Hoodoo_Startrail.jpg) |
| v17 | `v17-positive-orion-satellite-highway` | 至少 1 条 | 5 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Orion_satellite_highway.jpg) |
| v17 | `v17-positive-andromeda-tuntorp-2` | 至少 1 条 | 5 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Andromeda_Galaxy_over_a_rain_gutter_in_Tuntorp_2.jpg) |
| v17 | `v17-negative-leonid-meteor` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Leonid_Meteor.jpg) |
| v17 | `v17-negative-leonid-composite` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Leonid_Meteor_Shower_Composite_(50615053747).jpg) |
| v17 | `v17-negative-white-desert-startrail` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:White_Desert_star_trail.jpg) |
| v17 | `v17-negative-phone-startrails` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Star_trails_(50143160786).jpg) |
| v18 | `v18-positive-satellite-starry-night` | 至少 1 条 | 1 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Satellite_trail_in_a_starry_night_(175371665).jpg) |
| v18 | `v18-positive-milky-way-satellite` | 至少 1 条 | 1 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Milky_Way_and_satellite_trail.jpg) |
| v18 | `v18-negative-2009-leonid` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:2009_Leonid_Meteor_(4111291263).jpg) |
| v18 | `v18-negative-leonid-wipf` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Leonid_meteor_(50627764111).jpg) |
| v18 | `v18-negative-egyptian-desert` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:A_night_in_Egyptian_desert.jpg) |
| v18 | `v18-negative-star-trail-night-sky` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Star_trail_in_night_sky.jpg) |
| v19 | `v19-positive-rudd-pond-iridium` | 至少 1 条 | 3 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Rudd_Pond_at_night.jpg) |
| v19 | `v19-positive-iridium-tulsa` | 至少 1 条 | 1 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Iridium_Flare_Tulsa.jpg) |
| v19 | `v19-negative-flagstaff-meteor` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Night_sky_near_Flagstaff_with_meteor.jpg) |
| v19 | `v19-negative-geminid-fireball` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:The_Geminid_Fireball_(ann23043v).jpg) |
| v19 | `v19-negative-paranal-startrails` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Paranal_Starry_Night.jpg) |
| v19 | `v19-negative-rishikesh-startrail` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Star_trail_in_Rishikesh.jpg) |
| v20 | `v20-positive-iridium-flare` | 至少 1 条 | 3 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Iridium_flare.png) |
| v20 | `v20-positive-iridium-belfort` | 至少 1 条 | 14 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:2011-08-27_22-34-12-iridium-flare.jpg) |
| v20 | `v20-negative-perseid-2021` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:04-Perseid-2021-nX-5.jpg) |
| v20 | `v20-negative-meteor1` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Meteor1.jpg) |
| v20 | `v20-negative-iss-africa-startrails` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:ISS-65_African_city_lights,_atmospheric_glow_and_star_trails.jpg) |
| v20 | `v20-negative-star-trail-254221623` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Star_Trail_(254221623).jpeg) |
| v21 | `v21-positive-satellite-germany` | 至少 1 条 | 28 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Satellite_tracks_over_Germany_2018_(ann19035b).jpg) |
| v21 | `v21-positive-orion-satellites` | 至少 1 条 | 19 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Orion_Nebula_with_Satellite_Trails_(ann21021b).jpg) |
| v21 | `v21-positive-gaia-dotted` | 至少 1 条 | 1 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Pinpointing_Gaia_from_Earth_ESA19372289.jpg) |
| v21 | `v21-positive-teide-iss-startrails` | 1–8 条 | 1 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Circumpolar_star_trails_and_ISS_transit_over_the_Teide_volcano,_Tenerife,_Spain.jpg) |
| v21 | `v21-negative-fireball-kitt-peak` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Fireball_Over_Kitt_Peak_(iotw2522a).jpg) |
| v21 | `v21-negative-meteor080117` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Meteor080117.jpg) |
| v21 | `v21-negative-startrails-first` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Star_trails,_First_Place_(ann21047m).jpg) |
| v21 | `v21-negative-mars-startrail` | 最多 0 条 | 0 | 通过 | [Commons](https://commons.wikimedia.org/wiki/File:Mars_star_trail_-_Kanha_National_Park.jpg) |

## 桌面逐图覆盖

桌面原图由最终安装版 `/Applications/PhotonStack.app/Contents/MacOS/photonstack` 原地读取，不复制、不提交，也不作为训练数据。`伪影` 是检测候选数，`保护流星` 是被算法判为不能移除的候选数。

| 文件 | 伪影 | 保护流星 |
| --- | ---: | ---: |
| `DSC_6687.NEF` | 6 | 10 |
| `DSC_6688.NEF` | 5 | 9 |
| `DSC_6689.NEF` | 2 | 12 |
| `DSC_6690.NEF` | 2 | 13 |
| `DSC_6691.NEF` | 3 | 12 |
| `DSC_6692.NEF` | 2 | 10 |
| `DSC_6693.NEF` | 2 | 12 |
| `DSC_6694.NEF` | 2 | 16 |
| `DSC_6695.NEF` | 2 | 11 |
| `DSC_6696.NEF` | 2 | 8 |
| `DSC_6697.NEF` | 2 | 15 |
| `DSC_6698.NEF` | 1 | 15 |
| `DSC_6699.NEF` | 4 | 14 |
| `DSC_6700.NEF` | 4 | 14 |
| `PhotonStack-Empty-Layers-230d773.tiff` | 0 | 0 |
| `PhotonStack-Layer-Blue.png` | 1 | 0 |
| `PhotonStack-Layer-Red.png` | 1 | 0 |
| `PhotonStack-RAW-Restored-5d42bd9.tiff` | 2 | 13 |

## 如何复验和维护

```sh
cmake --build build/debug --target photonstack
python3 tools/scripts/evaluate-artifact-blindset.py \
  --manifest tests/artifact-blindset/manifest.json --split all --download \
  --json-report build/artifact-blindset/base-final.json
for manifest in tests/artifact-blindset/rounds/*.json; do
  round=$(basename "$manifest" .json)
  python3 tools/scripts/evaluate-artifact-blindset.py \
    --manifest "$manifest" --split evaluation --download \
    --json-report "build/artifact-blindset/${round}-final.json"
done
python3 tools/scripts/evaluate-artifact-blindset.py \
  --manifest tests/artifact-blindset/rounds/v21.json \
  --binary /Applications/PhotonStack.app/Contents/MacOS/photonstack \
  --split evaluation --desktop-dir "$HOME/Desktop" --require-desktop \
  --json-report build/artifact-blindset/desktop-final.json
python3 tools/scripts/generate-artifact-dataset-document.py
```

大规模集合的复验命令：

```sh
python3 tools/scripts/evaluate-artifact-blindset.py \
  --manifest tests/artifact-blindset/corpus-v22.json \
  --binary /Applications/PhotonStack.app/Contents/MacOS/photonstack \
  --cache build/artifact-corpus/v22-special-1280 \
  --split evaluation --workers 4 --quiet \
  --json-report build/artifact-corpus/v22-current.json
python3 tools/scripts/generate-artifact-ai-review-sheets.py
python3 tools/scripts/generate-artifact-ai-review.py
```

新增下一轮时，先选择未用于调参的新来源，写入新的 `rounds/vNN.json`，冻结图片哈希和修改前的检测器源码哈希，再进行第一次检测。第一次结果必须先写入 `history.json`，之后才允许针对失败样本修改算法。

更强的下一步是给桌面和网络图片建立人工审核的轨迹掩码或中心线标注，以测量召回率、误检率和清除残留，而不只比较数量。

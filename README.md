用 NVIDIA DLSS 5 Neural Renderer（DLSS NR）神经网络对**视频与图片做逐帧画质增强**的本地工具。
纯本地运算，不上传任何数据；浏览器操作界面，无需安装。

> 🤖 **关于本软件**：这是 **Vibe Coding（对话式 AI 辅助开发）** 的产物——界面、引擎与视频处理
> 流程均由 AI 在人类引导下编写、评审与反复调优完成，人类负责需求定义、效果测试与发布决策。
> 欢迎 fork 与改进。

**v1.3 更新亮点（前端优化）**
- **批量渲染队列**：可连续提交多个任务，每个任务使用提交时的参数快照独立执行
  （时间窗口/编码器/模型/全部增强参数/输出位置），自动按序渲染；队列面板支持
  ▲/▼ 调序、✕ 删除未开始任务，并实时显示当前正在渲染的文件名
- **任务完成即自动对比**：每个任务渲染完成都会自动把「原视频 ↔ 渲染后」载入视频
  对比模块（按该任务自己的起点偏移对齐）
- **视频对比模块**：双视频同帧播放（主从同步 + 起点偏移）、共用进度条/音量、
  左右拖动分隔逐帧对比、放大对比、默认静音、播完自动同时重播
- **图片对比**：放大窗口等比显示统一（留黑边不拉伸）、修复原图与渲染层错位及
  “切换视频后旧帧串入”的竞态
- **时间范围校验**：终点必须大于起点，非法窗口渲染前即拦截（前端 + 服务端双保险）
- **文件/文件夹选择器**：中文界面与高分屏适配；文件夹改用现代大窗口
- **正式退出**：页面底部中央红色电源键（⏻）= 停止渲染 → 清空缓存 → 结束进程
  （并尝试自动关闭页面）；仅关浏览器时 60 秒无活动也会自动清空帧缓存
- 日志与守护程序提示中文化

> **使用提示**：源码仓库不含模型等二进制大文件（GitHub 单文件 100MB 限制）。
> 完整发行版已打包在 **GitHub Releases**（含引擎、界面、模型、深度运行库与内置运行时），
> 下载解压即可用，无需安装/改名。详见下文「快速开始」。

---
<img width="1699" height="1220" alt="12742acb-ec8e-434c-b484-b19d7bb777ae" src="https://github.com/user-attachments/assets/d15e82f6-a9ea-43d3-8418-71b3e6f652d9" />



## 快速开始

### 路线 A：普通用户（免编译，推荐）

1. 打开本仓库 **Releases** 页面，下载最新版 **完整发行包**（如 `DLSS5NR_v1.3.zip`）；
2. 解压到任意目录（路径含中文也没问题）；
3. 双击 **（点击启动）Start_DLSS5NR.bat**；
4. 浏览器自动打开操作界面（地址 http://127.0.0.1:8777）；
5. 拖入/选择视频或图片 → 调参数 → 点「开始处理」/「渲染此图」；
6. 用完直接关闭黑色命令行窗口：服务停止，同时自动清理临时缓存。

完整发行包已内置：处理引擎、NR 模型、深度运行库、网页界面、便携 node/ffmpeg，解压即用。

### 路线 B：源码构建（开发者）

1. 克隆本仓库源码；
2. 到 **Releases** 下载 **完整发行包**，解压后将包内的 `models/`（NR 模型 + 转发器）与
   `core/depth/`（深度运行库）复制到源码根目录——文件名与位置已按引擎探测路径排好，无需改名；
3. 按下方「构建」一节用 MSVC 编译出 `core/dlss5nr_engine.exe`；
4. 确保 PATH 里有 `ffmpeg`/`ffprobe` 与 `node`，双击 `start_ui.bat` 运行。

### Releases 资产说明

| 资产 | 内容 | 适用 |
|---|---|---|
| 完整发行包 `DLSS5NR_v1.3.zip` | 引擎 + 界面 + NR 模型(fp16/fp8) + 深度运行库 + 便携 node/ffmpeg + 启动脚本 + 使用说明 | 所有用户：解压 → 双击启动 → 浏览器操作 |

包内 `models/` 与 `core/depth/` 已按引擎探测路径排好：`models/` 放 NR 模型与配套转发器
（`nvngx_dlssnr_fp16.dll` / `nvngx_dlssnr_fp8.dll` + `nvngx.dll_dlssnr_fp16.dll` /
`nvngx.dll_dlssnr_fp8.dll`，文件名勿改），`core/depth/` 为可选的深度推理运行库。

---

## 使用指南

### 界面功能导览（自上而下分栏）

**视频渲染（左栏）**
- 选择路径或将视频拖入（支持 mp4/mov/mkv/avi/webm/m4v 等）。拖入的文件会先复制到软件内部临时区；
- 输出路径可选，留空则输出到 `<软件目录>/outputs/`，文件名自动加 `nr_` 前缀。

**参数设置（左栏，决定画面效果）**
- **模型文件**：Auto / FP16 / FP8（选择见下方兼容表）
- **NR Preset / NR Style**：DLSS 官方预设档
- **NR Intensity**：增强强度（默认 1.0）
- **Local Tone / Local Structure / Skin Structure Strength**：局部色调 / 结构 / 皮肤细节
- **Automatic Mask / NR UI Correction**：自动遮罩 / 界面元素修正
- **Residual Multiplier**：残差倍率（更强细节，慎用）
- **Optical Flow Motion**：硬件光流(NV-OF)运动矢量（关闭可明显加快）
- **Depth Inference Interval**：深度推理间隔（0=关闭，越小越费时）
- **渲染次数**：整段重复渲染遍数（≥1）
- **预设管理**（顶部）：命名保存整套参数；选中预设可加载/删除；打开页面自动恢复上次参数。

**输出设置（中栏）**
- 编码器：h264_nvenc（默认）/ hevc_nvenc / libx264 / libx265
- 时间范围：只处理需要的片段可大幅节省时间，也可在播放器拖动后一键设为起点/终点
- **批量渲染队列**：可连续点击「开始处理」提交多个任务（各自参数快照独立），按序自动
  执行；下方「渲染队列」面板可 ▲/▼ 调序、✕ 删除未开始任务，进度区实时显示当前文件
- 预览播放器兼作"选帧器"：选中帧后可点「渲染当前帧进行预览」做单帧对比（走无损 444 管线）
- 页面底部中央的红色电源键（⏻）= **正常退出**：停止渲染 → 清空缓存 → 结束进程

**对比预览（中栏）**
鼠标在画面上左右拖动即可对比「渲染前 / 渲染后」（引擎内置抖动降色带）。

**视频对比（右栏，渲染完成自动载入）**
自动把「原视频 ↔ 渲染后」同帧播放：共用播放/暂停、进度条与音量（默认静音），
画面左右拖动分隔条逐帧对比；支持「放大对比」（点右上角「关闭 ✕」或 Esc 退出）；
渲染起点非 0 秒时原视频侧会自动按任务起点偏移对齐。

**图片渲染（右栏）**
拖入 png/jpg/jpeg/bmp/webp/tif、截图后 Ctrl+V 粘贴、或点「选择图片…」→「渲染此图」；
「放大对比」弹全屏窗口（Esc 关闭）；「保存渲染结果…」另存为 PNG。

**浮动日志（右下角）**
所有操作与错误记录在此（默认折叠，点标题展开），出错先看这里。

### 显卡兼容表

| 你的显卡 | 建议选择 | 说明 |
|---|---|---|
| 20 系 / 30 系 | FP16 或 Auto | FP16 可跑（无 FP8 硬件） |
| 40 系 | 先试 FP16，不行切 FP8 | 两者皆可试 |
| 50 系 | FP8 | 官方/社区兼容版 |
| 非 NVIDIA 独显 | — | 无法运行，需 NVIDIA 独显 + 新版驱动 |

选错不会损坏文件，最多报错退出。常见报错排查：
- 结束码 1 / NO_BINARY → 显卡不支持所选模型，切换另一档
- NGX / 光流(NV-OF) 初始化失败 → 驱动过旧，更新 NVIDIA 驱动
- 闪退 / 显存不足 → 显存不够或驱动异常
- D3D12 设备创建失败 → Windows 过旧或显卡过老

### 性能参考（2080Ti / fp16 / 默认参数）

720P ≈ 20–24 fps，1080P ≈ 10–12 fps；帧耗时大头是 NR 模型推理本身（1080P 约 45–48 ms/帧）。
渲染时显卡占用/功耗较高属正常——光流稠密化、残差混合、抖动等步骤都已 GPU 化。

### 常见问题

- **双击 bat 没反应**：看命令行报错；确认 tools/ 完整；杀软添加信任；端口 8777 被占会自动清理。
- **输出打不开**：HEVC/H.264 请用 PotPlayer / VLC / MPC。
- **临时缓存何时清理**：关掉黑窗口的瞬间自动清空；断电/强杀才留到下次启动兜底清。
- **预设存在哪**：浏览器本地存储里（非软件目录），换电脑需重新保存。

---

## 目录结构

```
core/                     C++ 处理引擎（D3D12 + ffmpeg 管道）
  main.cpp                入口：CLI、参数解析、渲染主循环
  d3d12_ctx.*             渲染上下文、纹理上传/回读
  dlssnr.*                NGX DLSS NR feature 加载与调用
  ngx_params.*            NGX 参数块构造
  nvof_flow.*             硬件光流(NV-OF, D3D11) → 稀疏网格
  densify_pass.*          D3D12 计算着色器：稀疏网格 → 全分辨率运动场
  blend_pass.*            GPU 残差混合 + Bayer 抖动降位
  depth_anything.*        深度推理（可选）
  video_pipe.*            ffmpeg 解码/编码子进程封装
  build.sh                MSVC 构建脚本
web/                      浏览器界面 + 本地服务（node，无第三方依赖）
server_guard.c            启动守护（关窗即清临时缓存）
start_ui.bat              开发环境启动脚本
```

## 构建（Windows）

要求：Visual Studio 2022（x64 `cl.exe`）、Windows 10 SDK、Git-Bash 或类似 bash。

```bash
cd core
bash build.sh          # 产物 core/dlss5nr_engine.exe
```

引擎通过 ffmpeg 子进程编解码，运行期还需要 PATH 里有 `ffmpeg`/`ffprobe` 与 `node`
（完整包里已内置，源码构建需自备）。

## 命令行参数

```bash
core/dlss5nr_engine.exe --input in.mp4 --output out.mp4 \
  --encoder h264_nvenc --residual-mult 1.0 --frame-guidance 3 \
  --end-time 5 --perf
```

常用：`--input --output --encoder --codec-args --pix-fmt --start-time --end-time
--preset --intensity --style --local-tone --local-structure --skin-structure
--auto-mask --ui-correction --residual-mult --frame-guidance --depth-interval
--dump-frame --frame-reset --hw-decode`。完整列表见 `--help`。

---

## 许可与致谢

本项目源码以 **GPL-3.0** 发布（见 `LICENSE`）。DLSSNR 的接入方式与参数方案参考了以下
开源项目，特此致谢：

- **Magpie** 的 DLSSNR 实验支持：开源 fork **SAOG0721/Magpie** 的 **`experimental` 分支**
  （https://github.com/SAOG0721/Magpie ，GPL-3.0；上游为 Blinue/Magpie）；
- **OptiScaler** 社区的 DLSSNR 支持实现（本工具早期调试所用的转发器 `nvngx.dll_dlssnr*.dll`
  源于其社区构建）。

界面、引擎与视频处理流程为本项目独立编写。

- `core/nvof/` 头文件：Copyright (c) 2018-2023 NVIDIA Corporation，宽松许可（见文件头）。
- `core/depth/onnxruntime_c_api.h`：ONNX Runtime 项目头文件，MIT 许可。
- **Releases 中的模型与转发器为社区/原作者作品**，打包发布前请自行确认其许可与
  NVIDIA 软件许可条款允许；请勿用于商业用途。

> 再分发或商用前，请自行完成对第三方组件的许可与合规核查。

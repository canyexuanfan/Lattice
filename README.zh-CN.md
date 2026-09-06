# Lattice

一个本地化、低资源占用的 Windows 桌面分类格子整理工具。

Lattice 让你把桌面图标、文件和文件夹分组到持久的"分类格子"
（lattice = 格子）里。分类在桌面上常驻，不依赖网络，可以随时展开
或收起而不打扰下方真实的 Explorer 桌面图标。

本仓库是 Lattice 的开源发布版本。专有的开发历史、内部自动化
脚本和 Windows 桌面集成细节未包含在公开源码中。

> 其他语言：[English](./README.md)

---

## 主要特性

- **原生 Windows 桌面层。** Lattice 使用 Win32、Direct2D、
  DirectWrite 与 Windows Shell COM，不依赖 Qt、.NET 或任何
  第三方 UI 框架。
- **持久的分类格子。** 每个分类都会跨重启和关机记住自己的
  位置、大小、展开/收起状态、透明度以及所包含的项目。
- **单一原件的桌面受管。** 把真实文件、文件夹、`.lnk` 或
  `.url` 从当前用户桌面或公共桌面拖入分类时，会把那份唯一
  原件移动到应用托管数据目录，不会通过复制或修改 Explorer
  坐标来隐藏它；拖回去时会把同一份原件恢复到它原本的桌面
  路径并定位到请求的 Explorer 网格单元。从桌面以外拖入的
  项目会保持原位置的引用关系。虚拟 Shell 项使用单独的、
  可逆的可见性事务。
- **桌面层感知。** Lattice 的分类窗口位于桌面层，因此任何
  普通应用窗口都能覆盖它们，但桌面处于前台时它们仍然可见。
  它们永远不会覆盖到普通应用窗口之上。
- **可配置。** 位置、大小、透明度、主题、分类列表与默认行为
  全部保存在用户本地 AppData 下的 JSON 配置中。

- **秒级超时的安装前置。** `PrepareToInstall` 先创建配置快照，
  再通过 `WM_QUIT` 请求正在运行的 Lattice 实例退出，并只等待
  一个短暂固定窗口。若窗口结束时进程仍在运行，再发送一次
  `WM_CLOSE` 并等待第二个短暂窗口。总等待时间被限定在秒级，
  超时立即返回中文错误提示，让用户从托盘退出后重试。不再
  出现分钟级阻塞弹窗、无限等待，也不会强杀进程。
- **静默更新退出。** 所有自绘模态消息循环（输入框、消息框、
  设置框）都会传播 `WM_QUIT`，因此更新请求即便发生在对话框
  打开期间，也能干净地结束外层 `App::Run` 并释放单实例互斥量。
- **带 journal 的受管存储。** 桌面文件、文件夹、`.lnk` 和
  `.url` 项目都走同一套移动事务。收纳、移出、配置回滚和崩溃
  恢复全过程都写入 journal，确保原始路径始终可恢复。公共桌面
  权限仅在 Windows 真正需要时通过文件操作 UI 请求。

## 环境要求

- Windows 10 或更高版本（x64）
- Visual Studio 2022 + "使用 C++ 的桌面开发" 工作负载
  （MSVC v143、Windows 10 SDK）
- 约 20 MB 磁盘空间用于安装

## 构建

Lattice 使用 MSBuild 和 Visual Studio 工程文件，`scripts/` 下
两个脚本覆盖标准工作流：

```powershell
# 编译 EXE 和 smoke 套件的 Debug 构建
.\scripts\build.ps1 -Configuration Debug

# 启用全程序优化的 Release 构建
.\scripts\build.ps1 -Configuration Release

# 生成 Inno Setup 安装包
.\scripts\package.ps1
```

输出二进制为 `x64\Debug\Lattice.exe` 或 `x64\Release\Lattice.exe`。

## 回归测试

Lattice 目前还没有独立的单元测试框架，回归覆盖由集成在
`src/testing/SmokeCommands.cpp` 中的 Win32 / Explorer / 窗口
层测试套件提供，统一通过 `scripts/smoke.ps1` 调用。运行器会：

- 构建 Debug
- 为本次运行创建带 GUID 后缀的隔离 Config / Data / Runs 目录
- 用 30 秒 watchdog 跑完各 smoke 模式
- 成功后清理运行目录，失败时保留证据目录

完整入口与约定见 `tests/README.md`。

## 工程目录

```
Lattice/
├── assets/         品牌素材（logo、安装包图片）
├── docs/           产品与架构文档
├── installer/      Windows 安装器的 Inno Setup 脚本
├── scripts/        构建、打包、运行与回归脚本（PowerShell）
├── src/            C++20 源码
│   ├── app/        进程启动、单实例、托盘图标
│   ├── config/     ConfigStore（JSON 持久化）
│   ├── desktop/    桌面扫描、布局、会话、watcher、快捷方式存储
│   ├── model/      OrganizerModel（MVC 模型头文件）
│   ├── rendering/  D2D 上下文、图标缓存、壁纸背景
│   ├── shell/      Shell COM 集成、放置目标、启动器
│   ├── testing/    内置 smoke 命令
│   ├── ui/         窗口、对话框、图标网格、拖影
│   └── util/       ComInit、PathUtil、StringUtil、Win32Error
├── tests/          测试入口与约定
├── Lattice.vcxproj Visual Studio 工程文件
├── LICENSE         GNU 通用公共许可证 v3
└── README.md       英文说明（本文件的中文对照版）
```

## 文档

- `docs/desktop-organizer-prd-architecture-roadmap.md` — 项目
  的产品需求、架构与路线图。

## 许可证

Lattice 以 **GNU 通用公共许可证 v3 (GPL-3.0-only)** 发布。
完整文本见 [`LICENSE`](./LICENSE)。

任何你分发的衍生作品也必须以 GPL-3.0 发布，并按相同条款公开
对应源码。实际含义参见 [GPL-3.0 FAQ](https://www.gnu.org/licenses/gpl-faq.html)。

## 贡献

欢迎提交 bug 报告、翻译与补丁。贡献即视为同意以 GPL-3.0
许可你的贡献。

## 商标

"Windows"、"Win32"、"Direct2D"、"DirectWrite"、"WIC"、"Inno Setup"
等名称均为各自所有者的商标。本仓库中仅用于识别用途，不代表
任何形式的背书。

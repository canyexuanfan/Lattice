# Lattice 测试入口

## 0.4.68 单格统一宿主切片

`Lattice.exe --smoke-unified-host-widget-slice`先锁定共享`WidgetView`契约：
每显示器DPI坐标、标题与图标命中、有序多选、Ctrl+A/F2/Delete动作、图标与
间隙释放分流、成组选中稳定排序、折叠/锁定状态，以及宿主视图不拥有HWND、
render target、壁纸或IconCache。完成实现后同一入口继续执行固定像素Oracle比较
和0.4.68规格要求的宿主资源A/B。

`Lattice.exe --smoke-unified-host-production`锁定0.4.69生产迁移合同：10个格子一次发布到同一个DesktopHost，生产独立Widget HWND为0；点击后键盘命令必须携带当前格子的稳定category/item identity；候选校验失败必须保留旧集合；全部view只复用宿主的target、壁纸、IconCache与OLE注册。另用过时持久DPI=288的隔离配置断言运行时view改取当前显示器DPI，且配置原值不被回写。

Lattice 的自动化回归不是独立的空测试工程。真实 Win32、Explorer、窗口层级和拖放场景由
`src/testing/SmokeCommands.cpp` 中的内置命令实现，并由 `scripts/smoke.ps1` 统一运行。

当前 runner 会：

- 构建 Debug；
- 为本轮创建唯一 GUID，并隔离 Config、Data、Runs；
- 依次运行当前清单中的全部 smoke 模式，其中 `--smoke-desktop-snapshot` 覆盖唯一快照、
  碰撞保留、稳定顺序、修订隔离及多格子共享一次扫描；
- 为每个模式设置 30 秒 watchdog；
- 成功时清理本轮目录，失败时保留证据目录。

运行方式：

```powershell
.\scripts\smoke.ps1
```

共享桌面模型的纯逻辑断言直接进入现有可执行 smoke，避免另建空测试工程；涉及真实 Win32、
Explorer 或窗口层级的验证继续使用对应生产路径 smoke。

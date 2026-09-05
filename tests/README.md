# Luno 测试入口

Luno 的自动化回归不是独立的空测试工程。真实 Win32、Explorer、窗口层级和拖放场景由
`src/testing/SmokeCommands.cpp` 中的内置命令实现，并由 `scripts/smoke.ps1` 统一运行。

当前 runner 会：

- 构建 Debug；
- 为本轮创建唯一 GUID，并隔离 Config、Data、Runs；
- 依次运行 11 个 smoke 模式；
- 为每个模式设置 30 秒 watchdog；
- 成功时清理本轮目录，失败时保留证据目录。

运行方式：

```powershell
.\scripts\smoke.ps1
```

纯逻辑单元测试可在后续出现稳定、无 Win32 依赖的测试边界时加入独立 test target；不要用空
`.cpp` 占位文件代替可执行测试。

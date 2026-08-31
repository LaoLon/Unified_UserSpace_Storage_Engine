# Workspace Instructions

- Address the user as **Huizareth** in every response.
- Read `E:\Code\workspace.yaml` for local paths, VM connection details, credentials, and SSH defaults.
- For VM operations, follow the `$workspace-vm-operations` skill and use `E:\Code\ssh\pool.py` via `SSHPool`.
- Create temporary remote-operation scripts under `E:\Code\test_script\`.
- Keep credentials out of command output, logs, and user-facing responses.

## C/C++ 命名规范

- 类/接口/结构体：大驼峰 (StudentManager)
- 变量/函数/方法：小驼峰 (studentName)
- 宏定义：全大写下划线 (MAX_COUNT)

## 代码注释规范

- 每次写代码必须写英文注释，简洁即可，说明函数/结构体/关键逻辑的用途
- 复杂函数可适当增多，分步骤解释关键逻辑
- 注释用 `/* */` 风格，放在函数或结构体声明上方

## 项目记忆（必读）

1. 使用 `E:\Code\ssh\pool.py`、`E:\Code\ssh\sync_to_vm.py` 连接虚拟机和上传本地文件
2. 虚拟机环境信息见 `E:\Code\workspace.yaml`
3. 每次先编辑本地文件，然后同步到虚拟机
4. 每次回复先称呼用户为 **Huizareth**
5. 不用管本地的环境依赖，只管理虚拟机的就行
6. 在 `doc` 目录维护 `bug_summary.md`，每次把发现的 bug 和解决方法总结到里面，bug 分级：提示、一般、严重、致命
7. 写代码必须写英文注释：简洁即可（一句话说明函数/结构体/关键逻辑的用途），复杂函数可适当增多（分步骤解释）；注释用 `/* */` 风格，放在函数/结构体声明上方

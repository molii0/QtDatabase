# Git 使用小抄

> 项目根目录已放好 `.gitignore`（build/、*.db、*.pro.user 等都被忽略）。

## 首次使用（已经帮你执行过，备查）

```bash
git init -b main          # 初始化，主分支叫 main
git add .                 # 把源码和文档加入暂存区（被忽略的不会加）
git status                # 检查：确认没有 build/、*.db 等
git commit -m "数据库模块：状态机+版本迁移+REST 接口(v3)"   # 首次提交
```

## 日常用到的命令

```bash
git status                # 看改了哪些文件
git diff                  # 看改动内容（未暂存）
git add 文件名            # 加入暂存区（或 git add . 全部）
git commit -m "说明这次改了什么"
git log --oneline         # 看提交历史
git log --oneline -5      # 最近 5 条
```

## 提交后发现自己改错了 / 想撤销

```bash
git restore 文件名        # 丢弃未提交的改动(危险，改的东西会没)
git reset --soft HEAD~1   # 撤销最后一次 commit，改动保留在暂存区
git reset --hard HEAD~1   # 撤销最后一次 commit 且丢弃改动(危险)
git show --stat HEAD      # 看最后一次提交改了哪些文件
```

## 提示

- `*.pro.user` 和 `.qtcreator/` 记的是你本机的 Qt 路径，已忽略，不要提交；
- `charge_platform.db` 已忽略：别人克隆后运行一次会自动建库 + 写演示数据；
- 想备份整个项目：只要 `git push` 到远程仓库（GitHub/Gitee）或复制 `.git` 所在的项目文件夹即可。

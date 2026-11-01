---
name: 代码审查
about: 所有改动都通过此模板提交，审查者按分级标准给意见
title: "[core|net|node] "
labels: ""
assignees: ""
---

## 改动内容

<!-- 一句话说清改了什么。示例：[net] 为 HttpServer 增加发送超时，避免慢客户端占满 worker -->

## 为什么改

<!-- 关联 issue / 缺陷现象 / 设计动机。没有动机的重构请说明收益 -->

Closes #

## 影响面

- [ ] 影响 `core`（hash / consistent_hash / lru_cache / singleflight）
- [ ] 影响 `net`（http_server / http_client / peer_pool）
- [ ] 影响 `node`（group / node / 对外 HTTP 协议）
- [ ] 仅测试 / 文档 / 构建

## 风险点与回滚

<!-- 最坏情况是什么？怎么回滚？ -->

## 作者自查

- [ ] 本地 `cmake --build` + `ctest` 全绿
- [ ] 本地跑过 ASan/UBSan（并发改动需另跑 TSan）
- [ ] 新增逻辑的测试**在不改代码时会失败**（已实际验证）
- [ ] 无 P0 级问题遗留
- [ ] P1 级问题已修复，或已登记 `TODO(负责人, 截止日期)`
- [ ] 涉及内部分支的注释解释了「为什么」而非「是什么」
- [ ] 公开接口的错误语义已在头文件注释中写明

## 高风险项逐条确认（命中即必须勾）

- [ ] **三态语义**：`found / not-found / error` 是否被正确区分？error 不会进负缓存
- [ ] **并发**：新增共享状态有明确归属的锁；无 ABBA 锁序；写者不会让读者看到半更新状态
- [ ] **资源**：新增 fd / socket / 线程在所有 return 与 throw 路径上都被释放
- [ ] **信任边界**：外部输入（header / path / body）没有直接影响内部行为
- [ ] **阻塞**：没有在 worker 线程上引入无超时的阻塞调用
- [ ] **生命周期**：`Stop()` 后可安全重启；无悬垂引用

## 审查者意见

<!-- 按 P0/P1/P2 标注，格式：
🔴 P0 **标题**
`file:line` 问题描述
**为什么**：...
**建议**：...
-->

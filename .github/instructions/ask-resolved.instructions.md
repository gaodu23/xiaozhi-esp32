---
applyTo: "**"
description: "每次回复结束后使用 askQuestions 工具询问用户问题是否解决"
---
在每次回复的最后，使用 #tool:vscode_askQuestions 工具向用户确认问题是否已解决。

提问格式如下：
- header: "resolved"
- question: "你的问题是否已解决？"
- options:
  - label: "是，已解决"（recommended: true）
  - label: "否，需要继续"

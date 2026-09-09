# 配置库接口

- `document.hpp`：读取文档、继承、选择与分层，保留来源路径/行号。
- `model.hpp`：模型 key 白名单、显式覆盖、schema 3 联动推导与 `build_model()`。

本模块输出 `DramSpec`，不执行请求，不拥有 Controller 队列、PHY 或存储数据。
调用方仍需分别构造流量、MemorySystem、ControllerOptions 与 StorageModelOptions。

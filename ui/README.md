## UI (optional)

如果需要做复杂配置，建议为模块开发一个独立 Android App（需要 root 写配置文件）。

本目录是 `ZyGadget`，用于配置 `zygisk_gadget` 模块。

### 配置文件

- Path: `/data/adb/modules/zygisk_gadget/config`
- Format:

```json
{
  "package": {
    "name": "com.example.app",
    "delay": 0,
    "mode": {
      "config": true
    }
  }
}
```

Frida Gadget 配置：

- Path: `/data/adb/modules/zygisk_gadget/frida-gadget.config`

Listen 模式：

```json
{
  "interaction": {
    "type": "listen",
    "address": "0.0.0.0",
    "port": 8086,
    "on_port_conflict": "fail",
    "on_load": "wait"
  }
}
```

Script 模式：

```json
{
  "interaction": {
    "type": "script",
    "path": "/data/local/tmp/hook.js"
  }
}
```

界面逻辑：
- 第一栏选择一个目标 App，写入 `package.name`
- 第二栏设置 `package.delay`、交互模式
- Listen 模式设置监听地址和端口
- Script 模式通过 root 文件浏览选择脚本路径，默认从 `/data/local/tmp` 打开



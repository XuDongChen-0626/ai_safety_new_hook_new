# 产品配置说明

`config/` 用于放置不同产品/现场网络环境的 Kconfig 片段，由
`project_config.json` 的 `Build.ExtraConf` / `Build.SysbuildConf` 引入：

```text
config/product_{ProductId}.conf
config/product_{ProductId}_sysbuild.conf
```

本工程默认 `ProductId = 18`。产品 18 和产品 101 使用 V2 板定义，
产品 71 使用 V3 板定义。切换产品时修改 `project_config.json` 中的
`ProductId`、`DeviceId`、`OtaTarget`，
并同步更新 `config/product_*.conf` 中的产品参数。

| 产品 | 板定义 | 默认 `device.no` |
| --- | --- | --- |
| 18 | `craner_hook_stm32h743vit6`（V2） | `504` |
| 101 | `craner_hook_stm32h743vit6`（V2） | `503` |
| 71 | `craner_hook_stm32h743vit6_v3`（V3） | 参数服务默认值 |

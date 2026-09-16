# 产品配置说明

`config/` 用于放置不同产品/现场网络环境的 Kconfig 片段，由
`project_config.json` 的 `Build.ExtraConf` / `Build.SysbuildConf` 引入：

```text
config/product_{ProductId}.conf
config/product_{ProductId}_sysbuild.conf
```

本工程当前 `ProductId = 71`（吊钩 AI 安全控制器）。
切换产品时修改 `project_config.json` 中的 `ProductId`、`OtaTarget`，
并同步更新 `config/product_*.conf` 中的产品参数。

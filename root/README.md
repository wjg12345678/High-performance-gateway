# Root Assets

`root/` 目录用于存放静态页面资源、样式文件和上传文件目录。

## 页面入口

- `/`：统一入口页
- `/login`：登录页
- `/register`：注册页
- `/login-error`：登录失败页
- `/register-error`：注册失败页
- `/welcome`：登录后欢迎页
- `/files`：文件管理页
- `/share`：公开文件页
- `/media`：辅助静态资源页

## 兼容别名

- `index.html`、`login.html`、`register.html` 等文件仍作为静态资源存在
- `judge.html`、`log.html`、`logError.html`、`registerError.html` 属于历史兼容页面
- 历史数字/CGI 路由需显式开启 `legacy_compat=1` 或 `TWS_LEGACY_COMPAT=1`

## 历史 Action 标志位

- 以下路径仅在 `legacy_compat=1` 时生效
- `0`：注册
- `1`：登录
- `2`：登录检测
- `3`：注册检测
- `5`：历史图片页
- `6`：历史视频页
- `7`：历史关注页

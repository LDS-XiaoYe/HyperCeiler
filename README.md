<div align="center">

<img src="\imgs\icon.webp" width="160" height="160" style="display: block; margin: 0 auto;" alt="icon" />

# HyperCeiler Next

### Make HyperOS Great Again!

简体中文&nbsp;&nbsp;|&nbsp;&nbsp;[繁體中文](/README_zh-HK.md)&nbsp;&nbsp;|&nbsp;&nbsp;[English](/README_en-US.md)&nbsp;&nbsp;|&nbsp;&nbsp;[Українська](/README_uk_UA.md)&nbsp;&nbsp;|&nbsp;&nbsp;[Português (Brasil)](/README_pt-BR.md)

</div>

## Next 分支说明

**HyperCeiler Next** 是基于现有 HyperCeiler 渐进演进的模块化 Hook 平台，采用 Native-first 路线：先实现 Native 插件，再扩展 Java/Kotlin Dex 插件。保留已有 Hook 和管理界面，逐步复用、拆分现有基础设施。

> **当前状态：架构重构开发分支。** 本分支以 `os4-personal` 的现有代码为基础，保留 LSPosed/Xposed 接入、Native Hook 与 Resolver、HyperOS 4 桌面适配及回归测试。目前仓库尚未包含 Next 插件 SDK、远程加载器、签名发布工具及插件管理页面，以下 Next 能力属于开发目标，不能视为已经可用。

### Next 目标能力

| 能力 | 设计目标 | 当前状态 |
|:---|:---|:---|
| Native Plugin SDK | 稳定 C ABI、API 版本和结构体大小检查，Hook 独立编译为 `.so` | 待实现 |
| 内置 Runtime | 插件注册、兼容性检查、生命周期与错误管理；断网也能启动 | 待实现 |
| 远程分发 | HTTPS 索引与插件包，支持 GitHub Releases 或静态存储 | 待实现 |
| 可信验证 | APK 内置可信公钥，验证覆盖 Manifest 与二进制的 Ed25519 签名及 SHA-256 | 待实现 |
| 安全缓存 | 版本化缓存、原子安装与激活，区分下载、验证、安装、激活和运行成功 | 待实现 |
| 管理与恢复 | 更新、启停、历史版本、Last Known Good、崩溃检测及紧急禁用 | 待实现 |
| Dex 插件 | 复用分发与验证协议，提供独立 Dex 执行后端 | 后续阶段 |

### 远程 Hook 的预期使用方式

发布者独立编译 `.so`，生成签名 `manifest.json`，上传插件文件并更新远程索引。Manager 主动检查兼容更新，下载并验证后安装到本地缓存；用户激活后，目标进程下次启动时加载对应版本。

**Native 插件更新或回滚需要重启目标进程；桌面 Hook 需要重启桌面，不要求重启手机。** 不在 Zygote 或宿主启动路径请求网络，不通过 `dlclose()` 撤销已执行的 Hook，不强制热替换运行中的 Native 插件。

签名与发布权限是 Native 插件的信任边界：插件进入宿主后没有进程级隔离。远程插件不能替换 Runtime 核心；未经可信签名或不符合宿主版本、APK 指纹、系统与 ABI 要求的插件应被拒绝。正式开放前需要完成真实设备上的加载、权限、动态链接、崩溃恢复和性能验证。

## 当前支持的版本

Android 15-16 的 HyperOS

## 使用前说明

请在 [LSPosed](https://github.com/LSPosed/LSPosed/releases) 中启用 HyperCeiler，然后在 HyperCeiler 应用内启用对应的功能，重启作用域（需要 Root 权限）。

本模块<b>不支持</b>`修改较多的第三方 Xiaomi HyperOS ROM`、`修改较多的系统软件`，以及`部分国际 Xiaomi HyperOS ROM`。

目前 HyperCeiler 是基于 Android 16 的 Xiaomi HyperOS 3.0 的手机端设备进行适配，覆盖不是很完整，需要不断测试和改进。

提交反馈前请注意是否已有相同反馈，避免给开发者造成困扰。花相同精力看相同反馈是一件很浪费时间的事情。

HyperCeiler 已停止维护 Android 11-13 的 MIUI ROM 和 Android 13/14 的 HyperOS 1.0/2.0 ROM。

> Android 11-12 的 MIUI ROM 请使用[此版本](https://github.com/ReChronoRain/Cemiuiler/releases/tag/1.3.130)。
>
> Android 13 的 MIUI ROM 和 HyperOS 1.0 ROM 请使用[此版本](https://github.com/Xposed-Modules-Repo/com.sevtinge.hyperceiler/releases/download/3866-2.5.156_20250118/HyperCeiler_2.5.156_20250118_3866_release_miui.apk)。
>
> Android 14 的 HyperOS 1.0/2.0 已归档，从 2.6.162 版本开始将移除对 Android 14 的支持。

## 项目规划

按阶段推进：Native 基础设施与独立测试插件 → 本地 Native Plugin Runtime → 可信远程分发 PoC → 真实桌面 Native Hook 迁移 → NestJS/MySQL 分发后端 → Dex Hook 插件化。

优先保留现有功能，每阶段完成可用的编译与测试。远程 Native 插件首个迁移目标为 HyperOS 4 系统桌面底栏；既有功能的支持范围与未来插件的兼容范围分别验证，不能因分支更名而视为扩大系统支持。

## 作用域包含的应用

<details>
    <summary>点击展开折叠的内容</summary>

| 应用名                | 包名                                 |
|:-------------------|:-----------------------------------|
| 系统框架               | system                             |
| 系统界面               | com.android.systemui               |
| 系统桌面               | com.miui.home                      |
| 系统更新               | com.android.updater                |
| Joyose             | com.xiaomi.joyose                  |
| 小米设置               | com.xiaomi.misettings              |
| 安全服务 (手机管家、平板管家)   | com.miui.securitycenter            |
| 壁纸                 | com.miui.miwallpaper               |
| 传送门                | com.miui.contentextension          |
| 弹幕通知               | com.xiaomi.barrage                 |
| 电话                 | com.android.incallui               |
| 电话服务               | com.android.phone                  |
| 电量与性能              | com.miui.powerkeeper               |
| 短信                 | com.android.mms                    |
| 截屏                 | com.miui.screenshot                |
| 日历                 | com.android.calendar               |
| 笔记                 | com.miui.notes                     |
| 浏览器                | com.android.browser                |
| 鲁班（MTB）            | com.xiaomi.mtb                     |
| 屏幕录制               | com.miui.screenrecorder            |
| 权限管理服务             | com.lbe.security.miui              |
| 设置                 | com.android.settings               |
| 搜狗输入法小米版           | com.sohu.inputmethod.sogou.xiaomi  |
| 天气                 | com.miui.weather2                  |
| 互联互通服务 (投屏)        | com.milink.service                 |
| 外部存储设备             | com.android.externalstorage        |
| 息屏与锁屏编辑 (万象息屏)     | com.miui.aod                       |
| 文件管理               | com.android.fileexplorer           |
| 系统服务组件             | com.miui.securityadd               |
| 下载管理               | com.android.providers.downloads.ui |
| 下载管理程序             | com.android.providers.downloads    |
| 相册                 | com.miui.gallery                   |
| 小米创作               | com.miui.creation                  |
| 小米互传               | com.miui.mishare.connectivity      |
| 小米相册-编辑            | com.miui.mediaeditor               |
| 小米云服务              | com.miui.cloudservice              |
| 小米智能卡              | com.miui.tsmclient                 |
| 小米钱包                | com.mipay.wallet                   |
| 讯飞输入法小米版           | com.iflytek.inputmethod.miui       |
| 应用包管理组件            | com.miui.packageinstaller          |
| 应用商店               | com.xiaomi.market                  |
| 智能助理               | com.miui.personalassistant         |
| 主题商店 (主题壁纸、壁纸与个性化) | com.android.thememanager           |
| 系统安全组件             | com.miui.guardprovider             |
| 相机                 | com.android.camera                 |
| 小爱翻译               | com.xiaomi.aiasst.vision           |
| 小米澎湃 AI 引擎         | com.xiaomi.aicr                    |
| 小爱视觉               | com.xiaomi.scanner                 |
| NFC 服务             | com.android.nfc                    |
| 音质音效               | com.miui.misound                   |
| 备份                 | com.miui.backup                    |
| 小米换机               | com.miui.huanji                    |
| MiTrustService     | com.xiaomi.trustservice            |
| HTML 查看器           | com.android.htmlviewer             |
| 通话管理               | com.android.server.telecom         |
| 万能遥控               | com.duokan.phone.remotecontroller  |
| Analytics          | com.miui.analytics                 |
| 小米社区               | com.xiaomi.vipaccount              |
| 语音唤醒               | com.miui.voicetrigger              |
| 录音机                | com.android.soundrecorder          |
| LPA                | com.miui.euicc                     |
| 小米SIM卡激活服务         | com.xiaomi.simactivate.service     |
| 跨屏协同服务             | com.xiaomi.mirror                  |

</details>

> 与 LSPosed 中推荐的作用域相同

## 交流 & 反馈群组

加入我们所创建的群组以反馈问题或是了解最新情况。

[![badge_trguild]][trguild_url]
[![badge_tcguild]][tcguild_url]
[![badge_telegram]][telegram_url]

## 为 HyperCeiler 贡献翻译

我们使用了 Hosted Weblate 进行项目翻译同步，您可以在[这里](https://hosted.weblate.org/engage/hyperceiler/)为 HyperCeiler 项目贡献翻译。

<a href="https://hosted.weblate.org/engage/hyperceiler/">
<img src="https://hosted.weblate.org/widget/hyperceiler/multi-auto.svg" alt="翻译状态" />
</a>

> 注：当新语言翻译进度大于或等于 85% 时，将会进入合并流程，如果已添加的语言，翻译进度小于或等于源文本的 30%，将会暂时被移除，直到重新满足翻译进度大于或等于 85%。

## 感谢

> HyperCeiler 使用了以下开源项目的部分或全部内容，感谢这些项目的开发者提供的大力支持（排名顺序不分先后）。

- [「Accompanist」 by Android Open Source Project, Google Inc.](https://google.github.io/accompanist)
- [「Android」 by Android Open Source Project, Google Inc.](https://source.android.google.cn/license)
- [「AndroidHiddenApiBypass」 by LSPosed](https://github.com/LSPosed/AndroidHiddenApiBypass)
- [「AndroidX」 by Android Open Source Project, Google Inc.](https://github.com/androidx/androidx)
- [「AutoSEffSwitch」 by HChenX](https://github.com/HChenX/AutoSEffSwitch)
- [「AntiAntiDefraud」 by MinaMichita](https://github.com/MinaMichita/AntiAntiDefraud)
- [「AutoNFC」 by GSWXXN](https://github.com/GSWXXN/AutoNFC)
- [「BypassSignCheck」 by Weverses](https://github.com/Weverses/BypassSignCheck)
- [「CorePatch」 by LSPosed](https://github.com/LSPosed/CorePatch)
- [「CustoMIUIzer」 by MonwF](https://github.com/MonwF/customiuizer)
- [「CustoMIUIzerMod」 by liyafe1997](https://github.com/liyafe1997/CustoMIUIzerMod)
- [「ClipboardList」 by HChenX](https://github.com/HChenX/ClipboardList)
- [「DexKit」 by LuckyPray](https://github.com/LuckyPray/DexKit)
- [「Disable app link verify」 by tehcneko](https://github.com/Xposed-Modules-Repo/io.github.tehcneko.applinkverify)
- [「DisableFlagSecure」 by LSPosed](https://github.com/LSPosed/DisableFlagSecure)
- [「DisableLogRequest」 by QueallyTech](https://github.com/QueallyTech/DisableLogRequest)
- [「EzXHelper」 by KyuubiRan](https://github.com/KyuubiRan/EzXHelper)
- [「EzHookTool」 by lingqiqi5211](https://github.com/lingqiqi5211/EzHookTool)
- [「FixMiuiMediaControlPanel」 by qqlittleice](https://github.com/qqlittleice/FixMiuiMediaControlPanel)
- [「FocusNotifLyric」 by wuyou-123](https://github.com/wuyou-123/FocusNotifLyric)
- [「ForegroundPin」 by HChenX](https://github.com/HChenX/ForegroundPin)
- [「FuckNFC」 by xiaowine](https://github.com/xiaowine/FuckNFC)
- [「Gson」 by Android Open Source Project, Google Inc.](https://github.com/google/gson)
- [「XiaomiHelper」 by HowieHChen](https://github.com/HowieHChen/XiaomiHelper)
- [「HideMiuiClipboardDialog」 by zerorooot](https://github.com/zerorooot/HideMiuiClipboardDialog)
- [「HyperFocusApi」 by ghhccghk](https://github.com/ghhccghk/HyperFocusApi)
- [「HyperSmartCharge」 by buffcow](https://github.com/buffcow/HyperSmartCharge)
- [「HyperStar」 by YunZiA](https://github.com/YunZiA/HyperStar)
- [「Kotlin」 by JetBrains](https://github.com/JetBrains/kotlin)
- [「MaxFreeForm」 by YifePlayte](https://github.com/YifePlayte/MaxFreeForm)
- [「MediaControlOpt」 by YuKongA](https://github.com/YuKongA/MediaControlOpt)
- [「MiuiFeature」 by MoralNorm](https://github.com/moralnorm/miui_feature)
- [「MiuiHomeR」 by qqlittleice](https://github.com/qqlittleice/MiuiHome_R)
- [「MIUI IME Unlock」 by RC1844](https://github.com/RC1844/MIUI_IME_Unlock)
- [「MIUIQOL」 by chsbuffer](https://github.com/chsbuffer/MIUIQOL)
- [「MiuiXXL」 by Wine-Network](https://github.com/Wine-Network/Miui_XXL)
- [「HyperOSXXL」 by YuKongA](https://github.com/YuKongA/HyperOS_XXL)
- [「MIUI 通知修复」 by tehcneko](https://github.com/Xposed-Modules-Repo/io.github.tehcneko.miuinotificationfix)
- [「ModemPro」 by Weverse](https://github.com/Weverses/ModemPro)
- [「NoStorageRestrict」 by DanGLES3](https://github.com/Xposed-Modules-Repo/com.github.dan.nostoragerestrict)
- [「PortalHook」 by Haocen2004](https://github.com/Haocen2004/PortalHook)
- [「PinningApp」 by HChenX](https://github.com/HChenX/PinningApp)
- [「RemoveMiuiSystemSelfProtection」 by gfbjngjibn](https://github.com/gfbjngjibn/RemoveMiuiSystemSelfProtection)
- [「SettingsDontThroughTheList」 by weixiansen574](https://github.com/weixiansen574/settingsdontthroughthelist)
- [「StarVoyager」 by hosizoraru](https://github.com/hosizoraru/StarVoyager)
- [「SuperLyric」 by HChenX](https://github.com/HChenX/SuperLyric)
- [「WINI」 by ouhoukyo](https://github.com/ouhoukyo/WINI)
- [「WOMMO」 by YifePlayte](https://github.com/YifePlayte/WOMMO)
- [「Woobox For MIUI」 by hosizoraru](https://github.com/hosizoraru/WooBoxForMIUI)
- [「Woobox For MIUI」 by Simplicity-Team](https://github.com/Simplicity-Team/WooBoxForMIUI)
- [「Xposed」 by rovo89, Tungstwenty](https://github.com/rovo89/XposedBridge)
- [「XposedBridge」 by rovo89](https://github.com/rovo89/XposedBridge)
- [「.xlDownload」 by Kr328](https://github.com/Kr328/.xlDownload)

[trguild_url]: https://t.me/cemiuiler_release

[badge_trguild]: https://img.shields.io/badge/TG-频道-4991D3?style=for-the-badge&logo=telegram

[tcguild_url]: https://t.me/cemiuiler_canary

[badge_tcguild]: https://img.shields.io/badge/TGCI-频道-4991D3?style=for-the-badge&logo=telegram

[telegram_url]: https://t.me/cemiuiler

[badge_telegram]: https://img.shields.io/badge/dynamic/json?style=for-the-badge&color=2CA5E0&label=Telegram&logo=telegram&query=%24.data.totalSubs&url=https%3A%2F%2Fapi.spencerwoo.com%2Fsubstats%2F%3Fsource%3Dtelegram%26queryKey%3Dcemiuiler

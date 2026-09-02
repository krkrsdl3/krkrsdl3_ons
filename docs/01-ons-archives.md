# ONScripter 封包与脚本格式研究（供集成复用）

来源：上游 Onscripter 源码（随仓库提供；NsaReader / SarReader / DirectReader / ScriptHandler 实现），
以及 NScripter 公开文档。本文件是后续 `ons_archive.cpp` 与引擎接入的实现依据。

## 1. 文件查找顺序（引擎视角）

`ScriptParser::openScript()` 创建 `NsaReader`（含 NS2 支持），失败则退回 `DirectReader`。
`ScriptHandler::openScript()` 依次尝试：`0.txt` → `00.txt` → `nscript.___`（key_table 加密）→
`nscript.dat`（0x84 加密或明文）→ `onscript.nt2` → `onscript.nt3`。

所有文件读取最终都走 `fopen`（DirectReader::fopen 拼 `archive_path + path`，大小写不敏感回退）。
这是 Android/Web 移植用 `fopen_ons` 钩子的原因，也是本项目可选的接入点（M2 暂直接用本地磁盘 demo 目录）。

## 2. NSA 封包（arc.nsa / arc1.nsa / arc2.nsa …）

加密变体扩展名 `.___`（key_table 逐字节映射）。头为**大端**。

```
num_of_files : u16
base_offset  : u32          （数据区起点；与文件内偏移相加得绝对偏移）
每个文件条目（依次）:
  name          : 以 0x00 结尾，文件名被存为大写（读取时把 a-z 升为大写）
  compression   : u8          （仅 NSA；SAR 无此字段）
  offset        : u32         （相对 base_offset）
  length        : u32         （压缩后长度 / 原始长度）
  original_length : u32       （仅 NSA；NBZ/SPB 时为 0，表示“延迟计算”）
```

- 压缩类型字节：0 = 无压缩；1 = NBZ(bzip2)；2 = LZSS；3 = SPB（NScr 图像专用）。
- 文件名无压缩类型字节时按扩展名推断：NBZ/SPB → 压缩；JPG/GIF → 无压缩。
- 一个游戏可以有多个 `arcN.nsa`（N 从 1 起；`arc.nsa` 为 0）。

### NS2 封包（arc00.ns2 …，NScr 2.91+）

```
base_offset : u32（字节序反转的 u32，即 swapLong(readLong)），之后
重复条目直到遇到非 '"' 字节：
  '"' name '"'  length : u32（swapLong）
之后是数据区。NS2 无压缩类型（一律无压缩），文件名条目内同样大写。
```

### SAR 封包（arc.sar）

```
num_of_files : u16
base_offset  : u32
每个条目: name\0  offset:u32  length:u32   （无压缩字段；original = length）
```

## 3. 压缩算法

### NBZ（类型 1）
```
u32 原始长度（大端）
bzip2 流（BZ2_bzReadOpen 于 offset 处）
```
DirectReader::decodeNBZ / encodeNBZ 有完整实现，可直接移植；依赖 libbz2。

### LZSS（类型 2，用于 SAR 条目）
EI=8, EJ=4, P=1 → N=256, F=17。位流小端读（getbit）。DirectReader::decodeLZSS 有实现。

### SPB（类型 3，NScr 专用图像压缩）
```
u16 width, u16 height（大端）
3 个平面（B,G,R），每平面按行光栅顺序做 4 像素一组的差分编码：
  组头 n = getbit(3)；n==0 → 重复当前 c 四次；n==7 → m = getbit(1)+1；否则 m = n+2
  每组 4 个像素：m==8 → 直接 getbit(8) 为新值；否则 k=getbit(m)，c ±= (k>>1)+1 / (k>>1)
输出为 24bit BMP（54 字节头 + 每行 4 字节对齐）。
```
DirectReader::decodeSPB 有实现，可直接移植。

## 4. 脚本文件加密（nscript.dat / nscript.___ / nt2 / nt3）

- **nscript.dat**：`ch = key_table[ch] ^ 0x84` 逐字节解密（默认 key_table 为恒等映射）；
  首字节特征判断是否加密（ScriptHandler::readScriptSub 中 encrypt_mode 1/2）。
- **nscript.___**：`key_table[ch]`（key 来自 `--key-exe` 的 EXE，createKeyTable 从 PE 中解出 256 字节表）。
- **nt2/nt3**：Mine 专用加密（encrypt_mode 4/5），上游 Onscripter 已实现。
- 脚本支持 GBK / SJIS / UTF-8（`coding2utf16`，默认 GBK；demo 若为日文需 `--enc:sjis`）。

## 5. 脚本语法要点（NScr）

- 行式命令，`;` 注释；`*label` 定义标签；`jump`/`goto` 跳转；`textgosub`/`gosub`。
- 文本行直接显示到文字区；`click`/`wait` 等待；`bg`/`ld`/`ls` 图片；`bgm`/`se`/`playstop` 音频；
  `setwindow` 文字窗；`select`/`button` 选择支；`mov`/`inc`/`if` 变量；`effect`/`effectblank` 特效；
  `rmenu` 右键菜单；`lua` 扩展（USE_LUA）。
- 命令表：`ONScripter_command.cpp`（`command_list[]`，约 200+ 命令）；解析器 `ScriptParser_command.cpp`。

## 6. 与 krkrsdl3 存储的映射（本项目 M1）

| ONScripter 概念 | krkrsdl3 概念 |
|---|---|
| `arc.nsa` 内文件 `BG/001.BMP` | `>arc.nsa>BG/001.BMP`（`>` 为 TVPArchiveDelimiter） |
| NSA/NS2/SAR 头与条目 | `tTVPArchive` 子类 + `TVPRegisterArchiveFormat` |
| NBZ/SPB/LZSS 解压 | 在 `CreateStreamByIndex` 里流式解压（tTJSBinaryStream 包装） |
| key_table 加密 | 仅 `.___`/加密变体需要；普通 arc.nsa 无 |
| nscript.dat 解密 | 引擎自持（ScriptHandler），不进存储层 |

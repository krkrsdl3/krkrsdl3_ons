/* -*- C++ -*-
 *  onscripter/ons_archive.cpp
 *
 *  ONScripter 封包 (NSA / NS2 / SAR) 在 krkrsdl3 存储系统中的接入。
 *
 *  思路（libretro）：ONScripter 特有的封包格式通过 TVPRegisterArchiveFormat
 *  注册为 krkrsdl3 的一等归档格式，此后 TJS 侧可以像使用 xp3 一样使用
 *   ">arc.nsa>bgimage/001.bmp" 形式的路径（Storage / LoadGraph / System.openStream）。
 *
 *  格式依据：docs/01-ons-archives.md（NsaReader / SarReader / DirectReader 移植）
 *
 *  GPL-2.0（源自 ONScripter 算法）参见 Onscripter/COPYING
 */

#include "tjsCommHead.h"
#include "TVPArchive.h"
#include "TVPStorage.h"
#include "tjsUtils.h"
#include "tjsInterface.h"

#include <bzlib.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 大端 / 位流读取（移植自 DirectReader）
// ---------------------------------------------------------------------------

static tjs_uint16 ReadBE16(const tjs_uint8* p)
{
    return (tjs_uint16)((p[0] << 8) | p[1]);
}

static tjs_uint32 ReadBE32(const tjs_uint8* p)
{
    return ((tjs_uint32)p[0] << 24) | ((tjs_uint32)p[1] << 16) | ((tjs_uint32)p[2] << 8) |
           (tjs_uint32)p[3];
}

// LZSS 参数（DirectReader）
#define ONS_LZSS_EI 8
#define ONS_LZSS_EJ 4
#define ONS_LZSS_P 1
#define ONS_LZSS_N (1 << ONS_LZSS_EI)     /* 256 */
#define ONS_LZSS_F ((1 << ONS_LZSS_EJ) + ONS_LZSS_P) /* 17 */

// 位流读取器（内存版 getbit）
struct tONSBitReader
{
    const tjs_uint8* Buf;
    size_t Size;
    size_t Pos; // 当前缓冲位置
    int Mask;   // 当前字节内的位掩码
    int Cur;    // 当前字节（key_table 未用）
    size_t Count; // 已消耗字节数

    tONSBitReader(const tjs_uint8* buf, size_t size) : Buf(buf), Size(size), Pos(0), Mask(0), Count(0)
    {
        Cur = 0;
    }

    int getbit(int n)
    {
        int i, x = 0;
        for (i = 0; i < n; i++)
        {
            if (Mask == 0)
            {
                if (Pos >= Size)
                    return EOF;
                Cur = Buf[Pos++];
                Mask = 128;
            }
            x <<= 1;
            if (Cur & Mask)
                x++;
            Mask >>= 1;
        }
        return x;
    }
};

// ---------------------------------------------------------------------------
// 解压算法
// ---------------------------------------------------------------------------

// NBZ: u32 原始长度(大端) + bzip2 流
static bool OnsNBZDecode(const tjs_uint8* src, size_t srcLen, std::vector<tjs_uint8>& dst,
                         tjs_uint64 hintLen)
{
    if (srcLen < 4)
        return false;
    tjs_uint64 original = ReadBE32(src);
    if (hintLen > original)
        original = hintLen;
    if (original > 512ull * 1024 * 1024) // 防御性上限
        return false;

    dst.resize((size_t)original);

    bz_stream bzs;
    memset(&bzs, 0, sizeof(bzs));
    bzs.next_in = (char*)src + 4;
    bzs.avail_in = (unsigned)srcLen - 4;

    int err = BZ2_bzDecompressInit(&bzs, 0, 0);
    if (err != BZ_OK)
        return false;

    size_t total = 0;
    bool ok = false;
    while (true)
    {
        if (total >= dst.size())
        {
            // 输出比声明的原始长度更长：扩容
            dst.resize(dst.size() * 2 + 4096);
        }
        bzs.next_out = (char*)dst.data() + total;
        bzs.avail_out = (unsigned)(dst.size() - total);

        err = BZ2_bzDecompress(&bzs);
        total = (size_t)(bzs.total_out_lo32 | ((tjs_uint64)bzs.total_out_hi32 << 32));
        if (err == BZ_STREAM_END)
        {
            ok = true;
            break;
        }
        if (err != BZ_OK)
            break;
        if (bzs.avail_in == 0 && bzs.avail_out > 0 && total == 0)
            break; // 输入耗尽且无输出
        if (bzs.avail_out > 0 && bzs.avail_in == 0)
            break; // 输入耗尽
    }
    BZ2_bzDecompressEnd(&bzs);
    if (!ok)
        return false;

    dst.resize(total);
    return true;
}

// LZSS（DirectReader::decodeLZSS 的内存版）
static bool OnsLZSSDecode(const tjs_uint8* src, size_t srcLen, std::vector<tjs_uint8>& dst,
                          tjs_uint64 expectedLen)
{
    tONSBitReader br(src, srcLen);
    std::vector<tjs_uint8> ring(ONS_LZSS_N * 2 + 16, 0);
    size_t r = ONS_LZSS_N - ONS_LZSS_F;

    if (expectedLen == 0 || expectedLen > 512ull * 1024 * 1024)
        expectedLen = srcLen * 4 + 4096;
    dst.reserve((size_t)expectedLen);

    size_t count = 0;
    while (expectedLen == 0 || count < expectedLen)
    {
        if (br.getbit(1))
        {
            int c = br.getbit(8);
            if (c == EOF)
                break;
            if (count >= dst.size())
                dst.resize(dst.size() * 2 + 4096);
            dst[count++] = (tjs_uint8)c;
            ring[r++] = (tjs_uint8)c;
            r &= (ONS_LZSS_N - 1);
        }
        else
        {
            int i = br.getbit(ONS_LZSS_EI);
            if (i == EOF)
                break;
            int j = br.getbit(ONS_LZSS_EJ);
            if (j == EOF)
                break;
            for (int k = 0; k <= j + ONS_LZSS_P; k++)
            {
                if (count >= dst.size())
                    dst.resize(dst.size() * 2 + 4096);
                tjs_uint8 c = ring[(i + k) & (ONS_LZSS_N - 1)];
                dst[count++] = c;
                ring[r++] = c;
                r &= (ONS_LZSS_N - 1);
            }
        }
    }
    dst.resize(count);
    return count > 0;
}

// SPB（DirectReader::decodeSPB 的内存版，输出 24bit BMP）
static bool OnsSPBDecode(const tjs_uint8* src, size_t srcLen, std::vector<tjs_uint8>& dst)
{
    if (srcLen < 4)
        return false;
    size_t width = ReadBE16(src);
    size_t height = ReadBE16(src + 2); // 高度在偏移 2（曾漏 +2 把高读成宽）
    if (width == 0 || height == 0 || width > 8192 || height > 8192)
    {
        fprintf(stderr, "[ONS] spb fail: w=%zu h=%zu\n", width, height);
        return false;
    }

    size_t width_pad = (4 - width * 3 % 4) % 4;
    size_t total_size = (width * 3 + width_pad) * height + 54;

    dst.resize(total_size);
    memset(dst.data(), 0, 54);
    dst[0] = 'B';
    dst[1] = 'M';
    dst[2] = total_size & 0xff;
    dst[3] = (total_size >> 8) & 0xff;
    dst[4] = (total_size >> 16) & 0xff;
    dst[5] = (total_size >> 24) & 0xff;
    dst[10] = 54;
    dst[14] = 40;
    dst[18] = width & 0xff;
    dst[19] = (width >> 8) & 0xff;
    dst[22] = height & 0xff;
    dst[23] = (height >> 8) & 0xff;
    dst[26] = 1;
    dst[28] = 24;
    dst[34] = total_size - 54;

    tONSBitReader br(src + 4, srcLen - 4);
    tjs_uint8* out = dst.data() + 54;

    std::vector<tjs_uint8> decomp(width * height);
    for (size_t i = 0; i < 3; i++)
    {
        size_t count = 0;
        int c = br.getbit(8);
        if (c == EOF)
            return false;
        decomp[count++] = (tjs_uint8)c;
        while (count < width * height)
        {
            int n = br.getbit(3);
            if (n == EOF)
                return false;
            if (n == 0)
            {
                for (int q = 0; q < 4 && count < width * height; q++)
                    decomp[count++] = (tjs_uint8)c;
                continue;
            }
            int m;
            if (n == 7)
            {
                int b = br.getbit(1);
                if (b == EOF)
                    return false;
                m = b + 1;
            }
            else
            {
                m = n + 2;
            }

            for (int j = 0; j < 4; j++)
            {
                if (m == 8)
                {
                    int v = br.getbit(8);
                    if (v == EOF)
                        return false;
                    c = v;
                }
                else
                {
                    int k = br.getbit(m);
                    if (k == EOF)
                        return false;
                    if (k & 1)
                        c += (k >> 1) + 1;
                    else
                        c -= (k >> 1);
                    if (c < 0)
                        c = 0;
                    if (c > 255)
                        c = 255;
                }
                if (count < width * height)
                    decomp[count++] = (tjs_uint8)c;
            }
        }

        tjs_uint8* pbuf = out + (width * 3 + width_pad) * (height - 1) + i;
        const tjs_uint8* psbuf = decomp.data();
        for (size_t j = 0; j < height; j++)
        {
            if (j & 1)
            {
                for (size_t k = 0; k < width; k++, pbuf -= 3)
                    *pbuf = *psbuf++;
                pbuf -= width * 3 + width_pad - 3;
            }
            else
            {
                for (size_t k = 0; k < width; k++, pbuf += 3)
                    *pbuf = *psbuf++;
                pbuf -= width * 3 + width_pad + 3;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 存档条目
// ---------------------------------------------------------------------------

namespace
{
enum
{
    // 与上游 BaseReader 对齐：NO_COMPRESSION=0, SPB=1, LZSS=2, NBZ=4
    ONS_C_NONE = 0,
    ONS_C_SPB = 1,
    ONS_C_LZSS = 2,
    ONS_C_NBZ = 4,
};

struct tTVPONSArchiveEntry
{
    std::string Name;          // 归一化后的名字（小写、'/'）
    tjs_uint64 Offset;         // 绝对偏移
    tjs_uint64 Length;         // 存储长度（压缩后）
    tjs_uint64 OriginalLength; // 原始长度（0 = 未知，需要解码时确定）
    int Compression;           // ONS_C_*
};

// 按扩展名推断压缩类型（对应 DirectReader 注册表）
int OnsCompressionFromExt(const std::string& name)
{
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return ONS_C_NONE;
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
    if (ext == "NBZ")
        return ONS_C_NBZ;
    if (ext == "SPB")
        return ONS_C_SPB;
    if (ext == "LZSS")
        return ONS_C_LZSS;
    return ONS_C_NONE;
}
} // namespace

// ---------------------------------------------------------------------------
// 自持有内存流（解码产物）
// ---------------------------------------------------------------------------

class tTVPONSMemoryStream : public tTJSBinaryStream
{
    tjs_uint8* Block;
    tjs_uint Size;
    tjs_uint CurrentPos;

public:
    tTVPONSMemoryStream(std::vector<tjs_uint8>&& buf)
        : Block(nullptr), Size((tjs_uint)buf.size()), CurrentPos(0)
    {
        Block = new tjs_uint8[Size ? Size : 1];
        if (Size)
            memcpy(Block, buf.data(), Size);
    }
    ~tTVPONSMemoryStream() override { delete[] Block; }

    tjs_uint64 Seek(tjs_int64 offset, tjs_int whence) override
    {
        switch (whence)
        {
            case TJS_BS_SEEK_SET:
                CurrentPos = (tjs_uint)offset;
                break;
            case TJS_BS_SEEK_CUR:
                CurrentPos = (tjs_uint)(offset + CurrentPos);
                break;
            case TJS_BS_SEEK_END:
                CurrentPos = (tjs_uint)(offset + Size);
                break;
        }
        if (CurrentPos > Size)
            CurrentPos = Size;
        return CurrentPos;
    }

    tjs_uint Read(void* buffer, tjs_uint read_size) override
    {
        if (CurrentPos + read_size > Size)
            read_size = Size - CurrentPos;
        if (read_size)
            memcpy(buffer, Block + CurrentPos, read_size);
        CurrentPos += read_size;
        return read_size;
    }

    tjs_uint Write(const void*, tjs_uint) override { return 0; }
    bool Flush() override { return true; }
    tjs_uint64 GetSize() override { return Size; }
    const std::string GetFileName() { return ""; }
};

// ---------------------------------------------------------------------------
// tTVPONSArchive
// ---------------------------------------------------------------------------

class tTVPONSArchive : public tTVPArchive
{
    tTJSBinaryStream* Stream; // 拥有
    std::vector<tTVPONSArchiveEntry> Entries;

    bool ReadExact(tjs_uint8* buf, tjs_uint len)
    {
        return Stream->Read(buf, len) == len;
    }

    bool ReadName(std::string& name)
    {
        // 读入以 0x00 结尾的文件名（大写），不做 key_table（普通 .nsa/.ns2/.sar）
        char ch;
        name.clear();
        while (true)
        {
            if (!ReadExact((tjs_uint8*)&ch, 1))
                return false;
            if (ch == 0)
                break;
            name += ch;
            if (name.size() > 512)
                return false;
        }
        return true;
    }

    // 归一化：大写→小写、'\\'→'/'、去重斜杠（对应 NormalizeInArchiveStorageName）
    void NormalizeName(std::string& name)
    {
        std::string out;
        out.reserve(name.size());
        bool prevSlash = false;
        for (char c : name)
        {
            if (c == '\\')
                c = '/';
            if (c == '/')
            {
                if (out.empty() || prevSlash)
                    continue;
                prevSlash = true;
            }
            else
            {
                prevSlash = false;
            }
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            out += c;
        }
        name = out;
    }

public:
    tTVPONSArchive(const ttstr& name, tTJSBinaryStream* st) : tTVPArchive(name), Stream(st) {}
    ~tTVPONSArchive() override { delete Stream; }

    tjs_uint GetCount() override { return (tjs_uint)Entries.size(); }

    ttstr GetName(tjs_uint idx) override
    {
        if (idx >= Entries.size())
            return ttstr();
        return ttstr(Entries[idx].Name.c_str());
    }

    tTJSBinaryStream* CreateStreamByIndex(tjs_uint idx) override
    {
        if (idx >= Entries.size())
            return nullptr;
        const tTVPONSArchiveEntry& e = Entries[idx];

        if (e.Compression == ONS_C_NONE)
        {
            // 未压缩：直接用基类子区间流（自动重开档案文件）
            return new TArchiveStream(this, e.Offset, e.Length);
        }

        // 压缩：读出原始字节并整体解码
        tjs_uint64 len = e.Length;
        if (len > 1ull << 30)
            return nullptr;
        std::vector<tjs_uint8> raw((size_t)len);

        tTJSBinaryStream* f = TVPGetCachedArchiveHandle(this, ArchiveName);
        if (!f)
            return nullptr;
        bool ok = false;
        f->SetPosition(e.Offset);
        if (f->Read(raw.data(), (tjs_uint)len) == len)
            ok = true;
        TVPReleaseCachedArchiveHandle(this, f);
        if (!ok)
            return nullptr;

        std::vector<tjs_uint8> out;
        switch (e.Compression)
        {
            case ONS_C_NBZ:
                if (!OnsNBZDecode(raw.data(), raw.size(), out, e.OriginalLength))
                    return nullptr;
                break;
            case ONS_C_LZSS:
                if (!OnsLZSSDecode(raw.data(), raw.size(), out, e.OriginalLength))
                    return nullptr;
                break;
            case ONS_C_SPB:
                if (!OnsSPBDecode(raw.data(), raw.size(), out))
                    return nullptr;
                break;
            default:
                return nullptr;
        }
        return new tTVPONSMemoryStream(std::move(out));
    }

    // ---- 解析（失败返回 false，不抛异常，调用方会继续尝试其他格式）----

    bool ParseNSA(tjs_uint64 fileSize)
    {
        tjs_uint8 hdr[6];
        if (!ReadExact(hdr, 6))
            return false;
        tjs_uint num = ReadBE16(hdr);
        tjs_uint64 base = ReadBE32(hdr + 2);
        if (num > 100000)
            return false;
        if (base >= fileSize)
            return false;

        Entries.reserve(num);
        for (tjs_uint i = 0; i < num; i++)
        {
            tTVPONSArchiveEntry e;
            if (!ReadName(e.Name))
                return false;
            tjs_uint8 comp;
            if (!ReadExact(&comp, 1))
                return false;
            tjs_uint8 off[4], len[4], olen[4];
            if (!ReadExact(off, 4) || !ReadExact(len, 4) || !ReadExact(olen, 4))
                return false;
            e.Compression = comp;
            e.Offset = ReadBE32(off) + base;
            e.Length = ReadBE32(len);
            e.OriginalLength = ReadBE32(olen);
            if (e.Offset + e.Length > fileSize)
                return false;
            if (e.Compression == ONS_C_NONE)
                e.Compression = OnsCompressionFromExt(e.Name);
            NormalizeName(e.Name);
            Entries.push_back(std::move(e));
        }
        return true;
    }

    bool ParseNS2(tjs_uint64 fileSize)
    {
        tjs_uint8 hdr[4];
        if (!ReadExact(hdr, 4))
            return false;
        tjs_uint32 base = ((tjs_uint32)hdr[3] << 24) | ((tjs_uint32)hdr[2] << 16) |
                          ((tjs_uint32)hdr[1] << 8) | hdr[0]; // 字节序反转
        if (base >= fileSize)
            return false;

        // 先数条目：'"' name '"' length(4, 反转)
        std::vector<std::pair<std::string, tjs_uint32>> names;
        while (true)
        {
            tjs_uint8 ch;
            if (!ReadExact(&ch, 1))
                return false;
            if (ch != '"')
                break;
            std::string name;
            while (true)
            {
                if (!ReadExact(&ch, 1))
                    return false;
                if (ch == '"')
                    break;
                name += (char)ch;
                if (name.size() > 512)
                    return false;
            }
            tjs_uint8 l4[4];
            if (!ReadExact(l4, 4))
                return false;
            tjs_uint32 len = ((tjs_uint32)l4[3] << 24) | ((tjs_uint32)l4[2] << 16) |
                             ((tjs_uint32)l4[1] << 8) | l4[0];
            names.emplace_back(name, len);
            if (names.size() > 100000)
                return false;
        }

        // 回到条目区起点重新解析（获得每个文件的偏移）
        // 条目区起点 = 4 + 每个条目的 (2 + name 长 + 4)
        // 简便起见：用与引擎相同的“从头再读”的方式
        Stream->SetPosition(4);
        tjs_uint64 cur = base;
        for (auto& pr : names)
        {
            tjs_uint8 ch;
            if (!ReadExact(&ch, 1) || ch != '"')
                return false;
            std::string name;
            while (true)
            {
                if (!ReadExact(&ch, 1))
                    return false;
                if (ch == '"')
                    break;
                name += (char)ch;
            }
            tjs_uint8 l4[4];
            if (!ReadExact(l4, 4))
                return false;
            tjs_uint32 len = ((tjs_uint32)l4[3] << 24) | ((tjs_uint32)l4[2] << 16) |
                             ((tjs_uint32)l4[1] << 8) | l4[0];

            tTVPONSArchiveEntry e;
            e.Name = name;
            e.Compression = OnsCompressionFromExt(name);
            e.Offset = cur;
            e.Length = len;
            e.OriginalLength = len;
            cur += len;
            if (cur > fileSize)
                return false;
            NormalizeName(e.Name);
            Entries.push_back(std::move(e));
        }
        return true;
    }

    bool ParseSAR(tjs_uint64 fileSize)
    {
        tjs_uint8 hdr[6];
        if (!ReadExact(hdr, 6))
            return false;
        tjs_uint num = ReadBE16(hdr);
        tjs_uint64 base = ReadBE32(hdr + 2);
        if (num > 100000)
            return false;
        if (base >= fileSize)
            return false;

        Entries.reserve(num);
        for (tjs_uint i = 0; i < num; i++)
        {
            tTVPONSArchiveEntry e;
            if (!ReadName(e.Name))
                return false;
            tjs_uint8 off[4], len[4];
            if (!ReadExact(off, 4) || !ReadExact(len, 4))
                return false;
            e.Compression = OnsCompressionFromExt(e.Name);
            e.Offset = ReadBE32(off) + base;
            e.Length = ReadBE32(len);
            e.OriginalLength = e.Length;
            if (e.Offset + e.Length > fileSize)
                return false;
            NormalizeName(e.Name);
            Entries.push_back(std::move(e));
        }
        return true;
    }

    // 名字排序（tTVPArchive 要求 GetName 按 ttstr::operator< 有序）
    void SortEntries()
    {
        std::vector<ttstr> names;
        names.reserve(Entries.size());
        for (auto& e : Entries)
            names.emplace_back(e.Name.c_str());
        std::vector<tjs_uint> order(Entries.size());
        for (tjs_uint i = 0; i < order.size(); i++)
            order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](tjs_uint a, tjs_uint b) { return names[a] < names[b]; });
        std::vector<tTVPONSArchiveEntry> sorted;
        sorted.reserve(Entries.size());
        for (tjs_uint i : order)
            sorted.push_back(std::move(Entries[i]));
        Entries = std::move(sorted);
    }
};

// ---------------------------------------------------------------------------
// creator（TVPRegisterArchiveFormat 注册）
// ---------------------------------------------------------------------------

static tTVPArchive* TVPOpenONSArchive(const ttstr& name, tTJSBinaryStream* st, bool)
{
    // 扩展名探测（ONS 封包无魔数）
    ttstr ext = TVPExtractStorageExt(name);
    bool isNSA = false, isNS2 = false, isSAR = false;
    if (ext == TJS_N(".nsa"))
        isNSA = true;
    else if (ext == TJS_N(".ns2"))
        isNS2 = true;
    else if (ext == TJS_N(".sar"))
        isSAR = true;
    else
        return nullptr; // 加密变体 "___" 暂不支持（需要 key_table）

    tjs_uint64 fileSize = 0;
    st->SetPosition(0);
    try
    {
        fileSize = st->GetSize();
    }
    catch (const TJS::eTJSError& e)
    {
        return nullptr;
    }
    catch (const std::exception& e)
    {
        return nullptr;
    }
    catch (...)
    {
        return nullptr;
    }
    if (fileSize < 16)
        return nullptr;

    tTVPONSArchive* arc = new tTVPONSArchive(name, st);
    bool ok = false;
    st->SetPosition(0);
    if (isNSA)
        ok = arc->ParseNSA(fileSize);
    else if (isNS2)
        ok = arc->ParseNS2(fileSize);
    else if (isSAR)
        ok = arc->ParseSAR(fileSize);

    if (!ok)
    {
        delete arc;
        return nullptr;
    }
    arc->SortEntries();
    return arc;
}

// ---------------------------------------------------------------------------
// 注册入口（由 onscripter 模块初始化时调用）
// ---------------------------------------------------------------------------

static void OnsAddArchiveAutoPaths();

void OnsRegisterArchiveFormats()
{
    TVPRegisterArchiveFormat(TVPOpenONSArchive);
    OnsAddArchiveAutoPaths();
}

// 把 arcN.nsa 注册为 krkrsdl3 自动搜索路径（虚拟文件系统）：
// TVPAddAutoPath("arc.nsa>") 后，裸文件名（NScr 惯例）经 TVPGetPlacedPath
// 自动落到封包内（TVPRebuildAutoPathTable 会枚举归档成员建表）。
void OnsAddArchiveAutoPaths()
{
    static const char* arcs[] = {"arc.nsa", "arc1.nsa", "arc2.nsa", "arc3.nsa", "arc4.nsa",
                                 "arc5.nsa"};
    for (auto a : arcs)
    {
        try
        {
            if (TVPIsExistentStorage(ttstr(a)))
                TVPAddAutoPath(ttstr(std::string(a) + ">"));
        }
        catch (...)
        {
        }
    }
}

void OnsUnregisterArchiveFormats()
{
    TVPUnregisterArchiveFormat(TVPOpenONSArchive);
}

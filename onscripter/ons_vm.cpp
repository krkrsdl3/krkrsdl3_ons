/* -*- C++ -*-
 *  onscripter/ons_vm.cpp
 *
 *  ONS 指令转发 VM 实现（docs/06）。
 *
 *  文本内部表示：**UTF-8**（与 krkrsdl3 一致，本移植 tjs_char = UTF-8 字节）。
 *  脚本解码：GBK/SJIS 双字节 -> UTF-16 -> UTF-8；UTF-8 源直接透传。
 *
 *  解码/解密参考上游 ScriptHandler::readScriptSub（GPL-2.0，见 Onscripter/COPYING）：
 *   - nscript.dat    : ch ^= 0x84
 *   - 0.txt/00.txt   : 明文
 *   - nscr_sec.dat   : 0x79 0x57 0x0d 0x80 0x04 循环异或
 *   - nscript.___    : key_table[ch] ^ 0x84（需 --key-exe，暂不支持）
 *   - onscript.nt2/3 : Mine 专用（暂不支持）
 *   CRLF 归一化为 LF。
 */

#include "ons_vm.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include "tjsCommHead.h"
#include "TVPStorage.h"
#include "tjsNativeLayer.h"
#include "PlatformView.h" // TVPSetWindowTitle

#include "ons_screen.h"
#include "ons_events.h"
#include "ons_audio.h"
#include "ons_movie.h"

#include "coding2utf16.h"
#include "gbk2utf16.h"
#include "sjis2utf16.h"

// ===========================================================================
// 工具（UTF-8 文本；ASCII 语法符号不受多字节影响）
// ===========================================================================

namespace
{

std::string ToLower(const std::string& s)
{
    std::string r = s;
    for (auto& c : r)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return r;
}

// 去掉首尾空白（含全角空格 0x3000 = UTF-8 EF BC 80）
std::string Trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    auto isWs = [](char c) { return c == ' ' || c == '\t'; };
    while (b < e && (isWs(s[b]) || (b + 2 < e && (unsigned char)s[b] == 0xef &&
                                    (unsigned char)s[b + 1] == 0xbc &&
                                    (unsigned char)s[b + 2] == 0x80)))
        b++;
    while (e > b &&
           (isWs(s[e - 1]) ||
            (e >= 3 && (unsigned char)s[e - 3] == 0xef && (unsigned char)s[e - 2] == 0xbc &&
             (unsigned char)s[e - 1] == 0x80)))
        e--;
    return s.substr(b, e - b);
}

// 按 ',' 或空白拆分参数（引号感知：引号内的逗号不拆分）
void SplitArgs(const std::string& s, std::vector<std::string>& out)
{
    out.clear();
    bool hasComma = false;
    bool inq = false;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '"')
            inq = !inq;
        else if (!inq && s[i] == ',')
        {
            hasComma = true;
            break;
        }
    }
    inq = false;
    std::string cur;
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '"')
        {
            inq = !inq;
            cur += c;
        }
        else if (!inq && c == ',')
        {
            out.push_back(Trim(cur));
            cur.clear();
        }
        else if (!inq && !hasComma && (c == ' ' || c == '\t'))
        {
            if (!cur.empty())
            {
                out.push_back(Trim(cur));
                cur.clear();
            }
        }
        else
        {
            cur += c;
        }
    }
    if (!cur.empty() || hasComma)
        out.push_back(Trim(cur));
    while (!out.empty() && out.back().empty())
        out.pop_back();

// 数字紧跟引号（dwave 0"w\s01.ogg" 无逗号形式）：拆成两个参数。
// 注意：必须先复制再 insert —— insert 可能触发重分配，使对 out[i] 的引用
// t 失效（此前 t 悬空导致 use-after-free，vcruntime 崩溃）。
    for (size_t i = 0; i < out.size(); i++)
    {
        const std::string t = out[i]; // 值拷贝，不随重分配失效
        if (t.size() >= 2 && t[0] >= '0' && t[0] <= '9')
        {
            size_t d = 0;
            while (d < t.size() && t[d] >= '0' && t[d] <= '9')
                d++;
            if (d < t.size() && t[d] == '"')
            {
                out[i] = t.substr(0, d);
                out.insert(out.begin() + i + 1, t.substr(d));
            }
        }
    }
}

// 去掉参数首尾引号（NScr 字符串参数常带 "）
std::string Unquote(const std::string& s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

// 引号外首个指定字符位置（npos = 无）
size_t FindUnquoted(const std::string& s, char target, size_t from = 0)
{
    bool inq = false;
    for (size_t i = from; i < s.size(); i++)
    {
        if (s[i] == '"')
            inq = !inq;
        else if (!inq && s[i] == target)
            return i;
    }
    return std::string::npos;
}

// 行内注释：引号外的 ';' 之后全部忽略（NScr ScriptHandler 语义）
std::string StripInlineComment(const std::string& s)
{
    size_t p = FindUnquoted(s, ';');
    return p == std::string::npos ? s : s.substr(0, p);
}

// 按 ':' 拆分为多条语句（引号感知；NScr 一行可含多条命令）
void SplitStatements(const std::string& s, std::vector<std::string>& out)
{
    out.clear();
    size_t start = 0;
    bool inq = false;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == '"')
        {
            inq = !inq;
            // 闭合引号后紧跟命令字：NScr 无分隔形式（dwave 0"w\s02.ogg"csp -1）
            if (!inq && i + 1 < s.size() &&
                ((s[i + 1] >= 'a' && s[i + 1] <= 'z') || (s[i + 1] >= 'A' && s[i + 1] <= 'Z')))
            {
                out.push_back(Trim(s.substr(start, i + 1 - start)));
                start = i + 1;
            }
        }
        else if (!inq && s[i] == ':')
        {
            out.push_back(Trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(Trim(s.substr(start)));
}

// 存档文件名：<dir>/ons<slot>.sav
std::string SaveSlotName(const std::string& dir, int slot)
{
    std::string d = dir;
    if (d.empty())
        d = "savedata/";
    if (d.back() != '/' && d.back() != '\\')
        d += '/';
    return d + "ons" + std::to_string(slot) + ".sav";
}

// 存储名解析：磁盘直读优先；失败则依次尝试 arc.nsa> / arc1.nsa> ...（NScr 惯例）。
// 注：TVPAddAutoPath("arc.nsa>") 只把归档顶层文件加入自动路径表（含 '/' 的
// 子目录条目会被 TVPRebuildAutoPathTable 跳过），子目录文件仍需显式探测。
// krkrsdl3 归档形式为 "arc.nsa>inner"（无前导 '>'）。名称归一化：\ -> /、小写。
std::string OnsResolveStorageName(const std::string& raw)
{
    if (raw.empty() || raw.find('>') != std::string::npos ||
        raw.find("://") != std::string::npos)
        return raw;
    std::string norm;
    for (char c : raw)
        norm += (c == '\\') ? '/' : c;
    std::string lower = ToLower(norm);
    if (TVPIsExistentStorage(ttstr(raw.c_str())))
        return raw; // 磁盘直读
    static const char* arcs[] = {"arc.nsa", "arc1.nsa", "arc2.nsa", "arc3.nsa", "arc4.nsa",
                                 "arc5.nsa"};
    for (auto a : arcs)
    {
        std::string cand = std::string(a) + ">" + lower;
        bool ok = false;
        try
        {
            // 打开验证（解压失败/空流 -> 视为不存在，避免 LoadImages 对
            // 无效流崩溃 —— krkrsdl3 TVPQuickTestBMP 对空流段错误）
            tTJSBinaryStream* st = TVPCreateStream(ttstr(cand.c_str()));
            if (st)
            {
                ok = st->GetSize() > 0;
                delete st;
            }
            else
                fprintf(stderr, "[ONS] resolve stream NULL: %s\n", cand.c_str());
        }
        catch (const std::exception& e)
        {
            fprintf(stderr, "[ONS] resolve stream EXC %s: %s\n", cand.c_str(), e.what());
        }
        catch (...)
        {
            fprintf(stderr, "[ONS] resolve stream EXC: %s\n", cand.c_str());
        }
        if (ok)
            return cand;
    }
    // 都不存在：返回空（调用方跳过 —— 避免 LoadImages 对无效文件崩溃
    // （krkrsdl3 TVPQuickTestBMP 对空流段错误））
    return "";
}

// 脚本字节 -> UTF-8 行
bool DecodeScript(const std::string& bytes, int encoding, std::vector<std::string>& lines,
                  std::string& error)
{
    Coding2UTF16* conv = nullptr;
    if (encoding == 1)
        conv = new SJIS2UTF16();
    else
    {
        conv = new GBK2UTF16();
        if (encoding == 2)
            conv->force_utf8 = true;
    }
    conv->init(); // 分配转换表（上游由 ONScripter::init 调用；缺失会导致 conv2UTF16 空指针）

    std::string cur;
    const unsigned char* p = (const unsigned char*)bytes.data();
    const unsigned char* end = p + bytes.size();

    // UTF-16 -> UTF-8 追加
    auto appendUtf8 = [&](unsigned short uc) {
        unsigned char buf[4];
        int n = conv->convUTF16ToUTF8(buf, uc);
        cur.append((const char*)buf, (size_t)n);
    };

    while (p < end)
    {
        unsigned char c = *p;
        if (c == 0x0a)
        {
            lines.push_back(cur);
            cur.clear();
            p++;
            continue;
        }
        if (c == 0x0d)
        {
            p++;
            continue;
        }
        if (c < 0x80)
        {
            cur += (char)c;
            p++;
            continue;
        }
        if (conv->force_utf8)
        {
            // UTF-8 序列：直接透传（与 krkrsdl3 底层一致）
            const char* cp = (const char*)p;
            unsigned short uc = conv->convUTF8ToUTF16(&cp);
            if (uc)
            {
                cur.append((const char*)p, (size_t)(cp - (const char*)p));
            }
            else
            {
                cur += (char)c;
                p++;
                continue;
            }
            p = (const unsigned char*)cp;
            if (p > end)
                p = end;
            continue;
        }
        // 双字节编码（GBK/SJIS）：首字节范围 0x81-0xFE（参考上游 IS_TWO_BYTE），
        // 越界字节（如被误读的 UTF-8 续字节）跳过，避免负下标访问转换表
        if (c >= 0x81 && c <= 0xfe && p + 1 < end)
        {
            unsigned short idx = ((unsigned short)c << 8) | p[1];
            appendUtf8(conv->conv2UTF16(idx));
            p += 2;
        }
        else
        {
            p++;
        }
    }
    if (!cur.empty())
        lines.push_back(cur);

    delete conv;

    if (lines.empty())
    {
        error = "empty script";
        return false;
    }
    return true;
}

// 简单表达式求值（递归下降，ASCII 语法）：
//   or -> and -> bitor -> bitxor -> bitand -> equality -> relational -> shift -> add -> mul -> unary -> primary
struct ExprParser
{
    const std::string& s;
    size_t pos = 0;
    const OnsVM* vm;

    explicit ExprParser(const std::string& str, const OnsVM* owner) : s(str), vm(owner) {}

    bool Parse(int& result) { return ParseOr(result) && SkipWs() && pos >= s.size(); }

    // 只解析表达式本身（不要求消费完；行内 if 用，EndPos() 取表达式结束位置）
    bool ParseExpr(int& result) { return ParseOr(result); }
    size_t EndPos() const { return pos; }

    bool SkipWs()
    {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t'))
            pos++;
        return true;
    }

    bool ParseOr(int& r)
    {
        if (!ParseAnd(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos + 1 < s.size() && s[pos] == '|' && s[pos + 1] == '|')
            {
                pos += 2;
                int rhs;
                if (!ParseAnd(rhs))
                    return false;
                r = (r || rhs) ? 1 : 0;
            }
            else
                break;
        }
        return true;
    }
    bool ParseAnd(int& r)
    {
        if (!ParseBitOr(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos + 1 < s.size() && s[pos] == '&' && s[pos + 1] == '&')
            {
                pos += 2;
                int rhs;
                if (!ParseBitOr(rhs))
                    return false;
                r = (r && rhs) ? 1 : 0;
            }
            else
                break;
        }
        return true;
    }
    bool ParseBitOr(int& r)
    {
        if (!ParseBitXor(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos < s.size() && s[pos] == '|' && !(pos + 1 < s.size() && s[pos + 1] == '|'))
            {
                pos++;
                int rhs;
                if (!ParseBitXor(rhs))
                    return false;
                r |= rhs;
            }
            else
                break;
        }
        return true;
    }
    bool ParseBitXor(int& r)
    {
        if (!ParseBitAnd(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos < s.size() && s[pos] == '^')
            {
                pos++;
                int rhs;
                if (!ParseBitAnd(rhs))
                    return false;
                r ^= rhs;
            }
            else
                break;
        }
        return true;
    }
    bool ParseBitAnd(int& r)
    {
        if (!ParseEquality(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos < s.size() && s[pos] == '&' && !(pos + 1 < s.size() && s[pos + 1] == '&'))
            {
                pos++;
                int rhs;
                if (!ParseEquality(rhs))
                    return false;
                r &= rhs;
            }
            else
                break;
        }
        return true;
    }
    // 字符串原子：'"'字面量（无反斜杠转义，遇下一个 '"' 或串尾止）或
    // '$'字符串变量（字母数字_，未定义时为空串——对齐 ：str 未赋值即 ""）。
    bool TryParseStringAtom(std::string& out)
    {
        SkipWs();
        if (pos >= s.size())
            return false;
        if (s[pos] == '"')
        {
            pos++;
            out.clear();
            while (pos < s.size() && s[pos] != '"')
                out += s[pos++];
            if (pos < s.size() && s[pos] == '"')
                pos++;
            return true;
        }
        if (s[pos] == '$')
        {
            size_t start = ++pos;
            while (pos < s.size() &&
                   ((s[pos] >= 'a' && s[pos] <= 'z') || (s[pos] >= 'A' && s[pos] <= 'Z') ||
                    (s[pos] >= '0' && s[pos] <= '9') || s[pos] == '_'))
                pos++;
            if (pos == start)
                return false;
            out.clear();
            vm->GetStrVar(s.substr(start, pos - start), out);
            return true;
        }
        return false;
    }

    bool ParseEquality(int& r)
    {
        // NScr 字符串比较：左值为 '"' 或 '$' 时按字符串（strcmp）语义比较
        // ——对齐 ifCommand：左值 STR 型 → readStr/strcmp。
        // 支持 == = != <> < <= > >=；比较后整条即字符串结果（可再接 && / ||）。
        {
            size_t save = pos;
            std::string lhs;
            if (TryParseStringAtom(lhs))
            {
                SkipWs();
                int op = 0; // 1:==/= 2:!=/<> 3:< 4:<= 5:> 6:>=
                size_t opLen = 0;
                if (pos + 1 < s.size() && s[pos] == '=' && s[pos + 1] == '=')
                {
                    op = 1;
                    opLen = 2;
                }
                else if (pos + 1 < s.size() && s[pos] == '!' && s[pos + 1] == '=')
                {
                    op = 2;
                    opLen = 2;
                }
                else if (pos + 1 < s.size() && s[pos] == '<' && s[pos + 1] == '>')
                {
                    op = 2;
                    opLen = 2;
                }
                else if (pos + 1 < s.size() && s[pos] == '<' && s[pos + 1] == '=')
                {
                    op = 4;
                    opLen = 2;
                }
                else if (pos + 1 < s.size() && s[pos] == '>' && s[pos + 1] == '=')
                {
                    op = 6;
                    opLen = 2;
                }
                else if (pos < s.size() && s[pos] == '=')
                {
                    op = 1;
                    opLen = 1;
                }
                else if (pos < s.size() && s[pos] == '<')
                {
                    op = 3;
                    opLen = 1;
                }
                else if (pos < s.size() && s[pos] == '>')
                {
                    op = 5;
                    opLen = 1;
                }
                if (op)
                {
                    pos += opLen;
                    SkipWs();
                    std::string rhs;
                    if (TryParseStringAtom(rhs))
                    {
                        int cmp = lhs.compare(rhs);
                        if (op == 1)
                            r = (cmp == 0) ? 1 : 0;
                        else if (op == 2)
                            r = (cmp != 0) ? 1 : 0;
                        else if (op == 3)
                            r = (cmp < 0) ? 1 : 0;
                        else if (op == 4)
                            r = (cmp <= 0) ? 1 : 0;
                        else if (op == 5)
                            r = (cmp > 0) ? 1 : 0;
                        else
                            r = (cmp >= 0) ? 1 : 0;
                        return true;
                    }
                }
            }
            pos = save; // 非字符串形态：回退整数路径
        }
        if (!ParseRelational(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos + 1 < s.size() && s[pos] == '=' && s[pos + 1] == '=')
            {
                pos += 2;
                int rhs;
                if (!ParseRelational(rhs))
                    return false;
                r = (r == rhs) ? 1 : 0;
            }
            else if (pos + 1 < s.size() && s[pos] == '!' && s[pos + 1] == '=')
            {
                pos += 2;
                int rhs;
                if (!ParseRelational(rhs))
                    return false;
                r = (r != rhs) ? 1 : 0;
            }
            else if (pos + 1 < s.size() && s[pos] == '<' && s[pos + 1] == '>')
            {
                pos += 2;
                int rhs;
                if (!ParseRelational(rhs))
                    return false;
                r = (r != rhs) ? 1 : 0;
            }
            else if (pos < s.size() && s[pos] == '=')
            {
                // NScr 惯例：单 '=' 即相等比较（if %911=1 / if %0=23 等）。
                // 无此分支时 "if %0=23" 退化为"取 %0 的值作条件"——恒进入
                // 第一个 if，其余分支永不命中（用户报告的按钮问题根因）。
                pos++;
                int rhs;
                if (!ParseRelational(rhs))
                    return false;
                r = (r == rhs) ? 1 : 0;
            }
            else
                break;
        }
        return true;
    }
    bool ParseRelational(int& r)
    {
        if (!ParseShift(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos + 1 < s.size() && s[pos] == '<' && s[pos + 1] == '=')
            {
                pos += 2;
                int rhs;
                if (!ParseShift(rhs))
                    return false;
                r = (r <= rhs) ? 1 : 0;
            }
            else if (pos + 1 < s.size() && s[pos] == '>' && s[pos + 1] == '=')
            {
                pos += 2;
                int rhs;
                if (!ParseShift(rhs))
                    return false;
                r = (r >= rhs) ? 1 : 0;
            }
            else if (pos < s.size() && s[pos] == '<')
            {
                pos++;
                int rhs;
                if (!ParseShift(rhs))
                    return false;
                r = (r < rhs) ? 1 : 0;
            }
            else if (pos < s.size() && s[pos] == '>')
            {
                pos++;
                int rhs;
                if (!ParseShift(rhs))
                    return false;
                r = (r > rhs) ? 1 : 0;
            }
            else
                break;
        }
        return true;
    }
    bool ParseShift(int& r)
    {
        if (!ParseAdd(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos + 1 < s.size() && s[pos] == '<' && s[pos + 1] == '<')
            {
                pos += 2;
                int rhs;
                if (!ParseAdd(rhs))
                    return false;
                r <<= rhs;
            }
            else if (pos + 1 < s.size() && s[pos] == '>' && s[pos + 1] == '>')
            {
                pos += 2;
                int rhs;
                if (!ParseAdd(rhs))
                    return false;
                r >>= rhs;
            }
            else
                break;
        }
        return true;
    }
    bool ParseAdd(int& r)
    {
        if (!ParseMul(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos < s.size() && s[pos] == '+')
            {
                pos++;
                int rhs;
                if (!ParseMul(rhs))
                    return false;
                r += rhs;
            }
            else if (pos < s.size() && s[pos] == '-')
            {
                pos++;
                int rhs;
                if (!ParseMul(rhs))
                    return false;
                r -= rhs;
            }
            else
                break;
        }
        return true;
    }
    bool ParseMul(int& r)
    {
        if (!ParseUnary(r))
            return false;
        while (true)
        {
            SkipWs();
            if (pos < s.size() && s[pos] == '*')
            {
                pos++;
                int rhs;
                if (!ParseUnary(rhs))
                    return false;
                r *= rhs;
            }
            else if (pos < s.size() && s[pos] == '/')
            {
                pos++;
                int rhs;
                if (!ParseUnary(rhs))
                    return false;
                r = rhs ? r / rhs : 0;
            }
            else if (pos < s.size() && s[pos] == '%')
            {
                pos++;
                int rhs;
                if (!ParseUnary(rhs))
                    return false;
                r = rhs ? r % rhs : 0;
            }
            else
                break;
        }
        return true;
    }
    bool ParseUnary(int& r)
    {
        SkipWs();
        if (pos < s.size() && s[pos] == '-')
        {
            pos++;
            if (!ParseUnary(r))
                return false;
            r = -r;
            return true;
        }
        if (pos < s.size() && s[pos] == '!')
        {
            pos++;
            if (!ParseUnary(r))
                return false;
            r = r ? 0 : 1;
            return true;
        }
        if (pos < s.size() && s[pos] == '~')
        {
            pos++;
            if (!ParseUnary(r))
                return false;
            r = ~r;
            return true;
        }
        return ParsePrimary(r);
    }
    bool ParsePrimary(int& r);
};

// 主表达式：数字 / %var% / 标识符 / (expr)
bool ExprParser::ParsePrimary(int& r)
{
    SkipWs();
    if (pos >= s.size())
        return false;
    char c = s[pos];

    if (c == '(')
    {
        pos++;
        if (!ParseOr(r))
            return false;
        SkipWs();
        if (pos >= s.size() || s[pos] != ')')
            return false;
        pos++;
        return true;
    }

    // %变量% / %变量（后跟非标识符时闭合 % 可省略，NScr 惯例）；?变量 同
    if (c == '%' || c == '?')
    {
        char prefix = c;
        size_t start = pos + 1;
        size_t e = start;
        while (e < s.size() &&
               ((s[e] >= 'a' && s[e] <= 'z') || (s[e] >= 'A' && s[e] <= 'Z') ||
                (s[e] >= '0' && s[e] <= '9') || s[e] == '_'))
            e++;
        if (e == start)
            return false;
        std::string name = (prefix == '?' ? "?" : "") + s.substr(start, e - start);
        pos = e;
        // 数组变量：?N[expr]（NScr 引数数组；CLANNAD ?1[1129] 等）。
        // 键 = "?N[下标]"，与 mov/set 命令的写路径（原样变量名）一致。
        if (pos < s.size() && s[pos] == '[')
        {
            pos++;
            int idx = 0;
            if (!ParseOr(idx))
                return false;
            SkipWs();
            if (pos >= s.size() || s[pos] != ']')
                return false;
            pos++;
            char buf[24];
            snprintf(buf, sizeof(buf), "[%d]", idx);
            name += buf;
        }
        if (pos < s.size() && s[pos] == '%')
            pos++; // 可选闭合 %
        int val;
        if (!vm->GetVar(name, val))
            return false;
        r = val;
        return true;
    }

    // 数字
    if (c >= '0' && c <= '9')
    {
        int v = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
        {
            v = v * 10 + (s[pos] - '0');
            pos++;
        }
        r = v;
        return true;
    }

    // 裸标识符：当作变量名
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
    {
        size_t start = pos;
        while (pos < s.size() &&
               ((s[pos] >= 'a' && s[pos] <= 'z') || (s[pos] >= 'A' && s[pos] <= 'Z') ||
                (s[pos] >= '0' && s[pos] <= '9') || s[pos] == '_'))
            pos++;
        std::string name = s.substr(start, pos - start);
        int val;
        if (!vm->GetVar(name, val))
            return false;
        r = val;
        return true;
    }

    return false;
}

// 解析矩形色块参数：>w,h,#color[,#color...]
bool ParseRectSpec(const std::string& arg, int& w, int& h, std::vector<uint32_t>& colors)
{
    if (arg.empty() || arg[0] != '>')
        return false;
    size_t i = 1;
    w = h = 0;
    while (i < arg.size() && arg[i] >= '0' && arg[i] <= '9')
        w = w * 10 + (arg[i++] - '0');
    if (i >= arg.size() || arg[i] != ',')
        return false;
    i++;
    while (i < arg.size() && arg[i] >= '0' && arg[i] <= '9')
        h = h * 10 + (arg[i++] - '0');
    while (i < arg.size())
    {
        if (arg[i] == ',' || arg[i] == ' ' || arg[i] == '\t')
        {
            i++;
            continue;
        }
        if (arg[i] == '#')
        {
            uint32_t c = 0;
            i++;
            for (int k = 0; k < 6 && i < arg.size(); k++, i++)
            {
                char hc = arg[i];
                int v = 0;
                if (hc >= '0' && hc <= '9')
                    v = hc - '0';
                else if (hc >= 'a' && hc <= 'f')
                    v = hc - 'a' + 10;
                else if (hc >= 'A' && hc <= 'F')
                    v = hc - 'A' + 10;
                else
                    break;
                c = (c << 4) | v;
            }
            colors.push_back(c);
        }
        else
            i++;
    }
    return w > 0 && h > 0 && !colors.empty();
}

// select 参数解析：按逗号拆分顶层 token，支持 "带引号字符串"（内部逗号不拆分）
bool SplitQuotedTokens(const std::string& s, std::vector<std::string>& out)
{
    out.clear();
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == ','))
            i++;
        if (i >= s.size())
            break;
        std::string tok;
        if (s[i] == '"')
        {
            i++;
            bool closed = false;
            while (i < s.size())
            {
                if (s[i] == '"')
                {
                    closed = true;
                    i++;
                    break;
                }
                tok += s[i++];
            }
            if (!closed)
                return false; // 引号未闭合
        }
        else
        {
            while (i < s.size() && s[i] != ',')
                tok += s[i++];
        }
        out.push_back(Trim(tok));
    }
    return true;
}

} // namespace
// 图片透明模式解析（上游 parseTaggedString + transmodeCommand）：
//   ":a;file"=TRANS_ALPHA（png 自带 alpha / jpg 图对）
//   ":l;file"=TRANS_TOPLEFT（挖左上角色） ":r;file"=TRANS_TOPRIGHT
//   ":c;file"/":m;file"=TRANS_COPY（原样；mask 无掩码文件时近似 COPY）
//   无前缀 = transmode 默认（CLANNAD DEFINE: transmode alpha）
int OnsVM::ResolveImageSpec(const std::string& raw, std::string& storage, int dflt)
{
    std::string body = raw;
    int mode = dflt;
    // transmode 默认映射（上游 transmodeCommand：alpha/leftcopy/rightcopy/copy）
    std::string tm = ToLower(m_transMode);
    if (tm == "topleft" || tm == "leftcopy")
        mode = OnsScreen::IMG_TOPLEFT;
    else if (tm == "topright" || tm == "rightcopy")
        mode = OnsScreen::IMG_TOPRIGHT;
    else if (tm == "copy")
        mode = OnsScreen::IMG_COPY;
    else
        mode = OnsScreen::IMG_ALPHA; // alpha / 未设置
    if (body.size() >= 2 && body[0] == ':')
    {
        char t = (char)ToLower(body.substr(1, 1))[0];
        size_t semi = body.find(';');
        if (t == 'a')
            mode = OnsScreen::IMG_ALPHA;
        else if (t == 'l')
            mode = OnsScreen::IMG_TOPLEFT;
        else if (t == 'r')
            mode = OnsScreen::IMG_TOPRIGHT;
        else if (t == 'c' || t == 'm')
            mode = OnsScreen::IMG_COPY;
        if (semi != std::string::npos)
            body = body.substr(semi + 1);
    }
    storage = OnsResolveStorageName(EvalStringExpr(body));
    return mode;
}



// ===========================================================================
// OnsVM
// ===========================================================================

void OnsVM::SetScreen(OnsScreen* screen)
{
    m_screen = screen;
    if (m_screen)
        m_screen->SetZOrder(m_zOrder); // DEFINE 段 humanz 落地（重排）
}

OnsVM::OnsVM() = default;
OnsVM::~OnsVM() = default;

bool OnsVM::LoadScriptFile(const std::string& name, int encoding)
{
    // 经 krkrsdl3 存储读取（项目目录/自动搜索路径/封包）
    tTJSBinaryStream* st = nullptr;
    try
    {
        st = TVPCreateStream(ttstr(name.c_str()));
    }
    catch (...)
    {
        st = nullptr;
    }
    if (!st)
    {
        m_error = "script not found: " + name;
        return false;
    }

    tjs_uint64 size64 = st->GetSize();
    if (size64 == 0 || size64 > 512ull * 1024 * 1024)
    {
        delete st;
        m_error = "script size invalid: " + name;
        return false;
    }
    std::string bytes((size_t)size64, 0);
    st->SetPosition(0);
    if (st->Read(&bytes[0], (tjs_uint)size64) != size64)
    {
        delete st;
        m_error = "script read failed: " + name;
        return false;
    }
    delete st;

    // 解密（参考上游 readScriptSub）
    int mode = 0;
    if (name == "nscript.dat")
        mode = 1;
    else if (name == "nscr_sec.dat")
        mode = 2;
    if (mode == 1)
    {
        for (auto& c : bytes)
            c = (char)((unsigned char)c ^ 0x84);
    }
    else if (mode == 2)
    {
        static const unsigned char magic[5] = {0x79, 0x57, 0x0d, 0x80, 0x04};
        int mc = 0;
        for (auto& c : bytes)
        {
            c = (char)(((unsigned char)c ^ magic[mc++]) & 0xff);
            if (mc == 5)
                mc = 0;
        }
    }

    // 编码转换 -> UTF-8 行（与 krkrsdl3 底层一致）
    m_lines.clear();
    if (!DecodeScript(bytes, encoding, m_lines, m_error))
        return false;
    return true;
}

bool OnsVM::Load(const std::string& scriptName, int encoding)
{
    m_error.clear();
    m_finished = false;
    m_loaded = false;

    // 脚本名：显式指定，或按 ONS 惯例探测
    static const char* candidates[] = {"0.txt", "00.txt", "nscr_sec.dat", "nscript.dat"};
    bool ok = false;
    if (!scriptName.empty())
    {
        ok = LoadScriptFile(scriptName, encoding);
    }
    else
    {
        for (auto c : candidates)
        {
            if (LoadScriptFile(c, encoding))
            {
                ok = true;
                break;
            }
        }
    }
    if (!ok)
        return false;

    // 标签表 + 首屏分辨率预扫描
    if (!PreScan())
        return false;

    // DEFINE 段设置（textgosub 等）+ 入口行（*start / game 之后）
    LoadDefineSection();

    m_loaded = true;
    return true;
}

void OnsVM::LoadDefineSection()
{
    // NScr：`game` 结束 DEFINE 段；设置项在加载时收集，不当作运行时代码执行；
    // 入口 = *start 标签（gameCommand -> resetCommand -> setCurrentLabel("start")），
    // 无 *start 则从 game 后一行开始。
    m_textGosubLabel.clear();
    m_rmenuItems.clear();
    int gameLine = -1;
    int startLine = -1;
    for (int i = 0; i < (int)m_lines.size(); i++)
    {
        std::string t = ToLower(Trim(m_lines[i]));
        if (t.empty() || t[0] == ';')
            continue;
        if (t[0] == '*')
        {
            if (startLine < 0 && t == "*start")
                startLine = i;
            continue;
        }
        if (gameLine < 0 && t == "game")
        {
            gameLine = i;
            continue;
        }
        if (gameLine < 0 || i < gameLine)
        {
            // DEFINE 段内设置
            if (t.rfind("textgosub", 0) == 0)
            {
                std::string rest = Trim(t.substr(9));
                if (!rest.empty() && rest[0] == '*')
                    m_textGosubLabel = rest.substr(1);
            }
            else if (t.rfind("pretextgosub", 0) == 0)
            {
                std::string rest = Trim(t.substr(12));
                if (!rest.empty() && rest[0] == '*')
                    m_pretextLabel = rest.substr(1);
            }
            else if (t.rfind("rmenu", 0) == 0)
            {
                // rmenu "显示",windowerase,"回看",lookback,...（引号感知拆分）
                std::vector<std::string> toks;
                if (SplitQuotedTokens(Trim(t.substr(5)), toks))
                {
                    for (size_t k = 0; k + 1 < toks.size(); k += 2)
                    {
                        RMenuAction a;
                        a.label = toks[k];
                        a.action = ToLower(toks[k + 1]);
                        m_rmenuDefItems.push_back(a);
                    }
                }
                m_rmenuItems = m_rmenuDefItems;
            }
            else if (t.rfind("menusetwindow", 0) == 0)
            {
                // 菜单字体：menusetwindow 字号,字号,行距增量,列距增量,bold,shadow[,#颜色]
                std::vector<std::string> a;
                SplitArgs(Trim(t.substr(13)), a);
                auto toInt = [&](size_t i, int d) {
                    int v = d;
                    if (i < a.size())
                        EvalExpression(a[i], v);
                    return v;
                };
                int fs = toInt(0, 18);
                m_menuFontSize = fs > 0 ? fs : 18;
                m_menuPitch = m_menuFontSize + toInt(2, 0);
            }
            else if (t.rfind("setwindow", 0) == 0)
            {
                // setwindow：DEFINE 段即生效（上游在预扫描/加载阶段执行；
                // 此时 screen 未就绪，仅记录参数，start() 创建文本层后应用）
                ExecuteStatement(m_lines[i]);
            }
            else if (t.rfind("numalias", 0) == 0)
            {
                // numalias 名,数字：%名 引用 -> %数字（上游 ScriptParser）
                std::vector<std::string> a;
                SplitArgs(Trim(t.substr(8)), a);
                if (a.size() >= 2)
                {
                    std::string name = a[0];
                    if (!name.empty() && name[0] == '%')
                        name = name.substr(1);
                    int v = 0;
                    EvalExpression(a[1], v);
                    m_numAliases[name] = std::to_string(v);
                }
            }
            else if (t.rfind("humanz", 0) == 0)
            {
                std::vector<std::string> a;
                SplitArgs(Trim(t.substr(6)), a);
                int z = 499;
                if (!a.empty())
                    EvalExpression(a[0], z);
                m_zOrder = z;
            }
            else if (t.rfind("defsub", 0) == 0)
            {
                // defsub 名：子例程名注册（行首名字单独出现 = gosub）
                std::vector<std::string> a;
                SplitArgs(Trim(t.substr(6)), a);
                if (!a.empty())
                    m_defsubs.insert(ToLower(a[0]));
            }
            else if (t.rfind("menuselectcolor", 0) == 0)
            {
                // 菜单颜色：menuselectcolor 悬停色,普通色,无效色
                std::vector<std::string> a;
                SplitArgs(Trim(t.substr(15)), a);
                auto color = [&](size_t i, uint32_t d) {
                    if (i >= a.size() || a[i].empty() || a[i][0] != '#')
                        return d;
                    uint32_t c = 0;
                    for (size_t k = 1; k < a[i].size() && k <= 6; k++)
                    {
                        char hc = a[i][k];
                        int v = 0;
                        if (hc >= '0' && hc <= '9') v = hc - '0';
                        else if (hc >= 'a' && hc <= 'f') v = hc - 'a' + 10;
                        else if (hc >= 'A' && hc <= 'F') v = hc - 'A' + 10;
                        else break;
                        c = (c << 4) | v;
                    }
                    return c;
                };
                m_menuOnColor = color(0, 0xffffff);
                m_menuOffColor = color(1, 0xb1b1b1);
            }
            else if (t == "globalon")
            {
                // globalon（上游 globalonCommand）：全部变量全局持久化到 gloval.sav
                m_globalon = true;
            }
            else if (t.rfind("effect", 0) == 0)
            {
                // DEFINE 段特效注册：effect no,effect[,duration[,image]]
                // （上游 effectCommand + readEffect；no=注册键，effect=特效编号）
                std::vector<std::string> a;
                SplitArgs(Trim(t.substr(6)), a);
                auto toInt = [&](size_t i, int d) {
                    int v = d;
                    if (i < a.size() && !a[i].empty())
                        EvalExpression(a[i], v);
                    return v;
                };
                int no = toInt(0, 0);
                int e = toInt(1, 0);
                int dur = toInt(2, 0);
                if (no >= 2 && no <= 255)
                    m_effects[no] = std::make_pair(e, dur);
            }
        }
    }
    if (startLine >= 0)
        m_pc = startLine;
    else if (gameLine >= 0)
        m_pc = gameLine + 1;
    else
        m_pc = 0;
}

bool OnsVM::PreScan()
{
    m_labels.clear();
    // 屏幕分辨率：默认 640x480（NScr 惯例）。可被以下覆盖（优先级从低到高）：
    //  1) 文件头注释的分辨率注记 ";mode800" / ";mode800,value500"
    //     （ever17 中文移植版脚本首行即此 —— 泛用解析，无需 startup.tjs 硬编码）；
    //  2) setwindow 首参 '$' 配置（setwindow $sW,H / $mode800）。
    m_screenW = 640;
    m_screenH = 480;
    bool setwindowScanned = false;
    for (int i = 0; i < (int)m_lines.size(); i++)
    {
        std::string line = Trim(m_lines[i]);
        if (line.empty())
            continue;
        if (line[0] == ';')
        {
            // 头部注释注记：";mode800" 或 ";mode800,value500" 等
            // （仅看前 8 行，避免正文注释误伤）
            if (i < 8)
            {
                std::string low = ToLower(line);
                size_t mp = low.find("mode");
                if (mp != std::string::npos)
                {
                    size_t q = mp + 4;
                    int v = 0;
                    while (q < low.size() && low[q] >= '0' && low[q] <= '9')
                    {
                        v = v * 10 + (low[q] - '0');
                        q++;
                    }
                    if (v == 1024)
                        m_screenW = 1024, m_screenH = 768;
                    else if (v == 800)
                        m_screenW = 800, m_screenH = 600;
                    else if (v == 400)
                        m_screenW = 400, m_screenH = 300;
                    else if (v == 320)
                        m_screenW = 320, m_screenH = 240;
                }
            }
            continue;
        }
        if (line[0] == '*')
        {
            m_labels[ToLower(line.substr(1))] = i;
            continue;
        }
        if (i < 2000)
        {
            std::string lower = ToLower(line);
            if (lower.rfind("setwindow", 0) == 0)
            {
                std::string args = Trim(line.substr(9));
                if (!setwindowScanned && !args.empty() && args[0] == '$')
                {
                    // 解析 $modeNNN / $sW,H 配置（参考上游 ScriptHandler 配置解析）
                    size_t p = 1;
                    while (p < args.size())
                    {
                        char c = args[p];
                        if (c == ' ' || c == '\t' || c == ',')
                        {
                            p++;
                            continue;
                        }
                        if (c == 'm' || c == 'M')
                        {
                            p++;
                            int w = 0, h = 0;
                            bool ok = false;
                            if (args.compare(p, 4, "1024") == 0)
                                w = 1024, h = 768, ok = true, p += 4;
                            else if (args.compare(p, 3, "800") == 0)
                                w = 800, h = 600, ok = true, p += 3;
                            else if (args.compare(p, 3, "400") == 0)
                                w = 400, h = 300, ok = true, p += 3;
                            else if (args.compare(p, 3, "320") == 0)
                                w = 320, h = 240, ok = true, p += 3;
                            if (ok)
                            {
                                m_screenW = w;
                                m_screenH = h;
                            }
                        }
                        else if (c == 's' || c == 'S')
                        {
                            p++;
                            int w = 0, h = 0;
                            bool okw = false;
                            while (p < args.size() && args[p] >= '0' && args[p] <= '9')
                            {
                                w = w * 10 + (args[p] - '0');
                                p++;
                                okw = true;
                            }
                            while (p < args.size() && (args[p] == ',' || args[p] == ' '))
                                p++;
                            while (p < args.size() && args[p] >= '0' && args[p] <= '9')
                            {
                                h = h * 10 + (args[p] - '0');
                                p++;
                            }
                            if (okw && w > 0 && h > 0)
                            {
                                m_screenW = w;
                                m_screenH = h;
                            }
                        }
                        else if (c == 'v' || c == 'V' || c == 'g' || c == 'G' || c == 'l' ||
                                 c == 'L')
                        {
                            p++;
                            while (p < args.size() && args[p] >= '0' && args[p] <= '9')
                                p++;
                        }
                        else
                        {
                            break;
                        }
                    }
                }
                else if (m_windowImage.empty() && !m_windowColorMode)
                {
                    // 非 $ 数字格式 setwindow（运行时段，如 splash 段的窗口设置）：
                    // 预扫描时执行一次以记录窗口参数（文本位置/字号/窗背图），
                    // 供自动读档（跳过 splash）后仍有正确文本框。
                    ExecuteStatement(line);
                }
            }
        }
    }
    return true;
}

bool OnsVM::Step()
{
    if (!m_loaded || m_finished)
        return false;
    FlushGlovalState(false); // gloval.sav 随时保存（去抖）
    if (m_trace && m_wait != WM_NONE)
        fprintf(stderr, "[ONS] -- wait=%d pc=%d\n", (int)m_wait, m_pc);

    // 鼠标移动：更新 exbtn 悬停效果（任意等待态都响应；
    // 非 motion 事件放回队列给下方等待分支处理）
    {
        OnsEvent ev;
        while (OnsPollEvent(&ev))
        {
            if (ev.type == ONS_EVENT_MOUSEMOTION)
            {
                UpdateHover(ev.u.motion.x, ev.u.motion.y);
                continue;
            }
            if (ev.type == ONS_EVENT_QUIT)
            {
                m_finished = true;
                return false;
            }
            // Ctrl 按住状态跟踪（任意等待态）：down 放回由等待分支作为点击
            // 推进；up 在此消费（等待分支忽略 keyup，不消费会滞留）
            if (ev.type == ONS_EVENT_KEYDOWN &&
                (ev.u.key.keysym.sym == ONSK_LCTRL || ev.u.key.keysym.sym == ONSK_RCTRL))
            {
                m_ctrlHeld = true;
                OnsPushEvent(&ev); // 等待分支当 v=0 / advance 处理
                break;
            }
            if (ev.type == ONS_EVENT_KEYUP &&
                (ev.u.key.keysym.sym == ONSK_LCTRL || ev.u.key.keysym.sym == ONSK_RCTRL))
            {
                m_ctrlHeld = false;
                continue; // 消费：不进入等待分支
            }
            if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN)
            {
                // 上游 mousePressEvent：任何鼠标按下先清 skip_mode（rmenu
                // 快速前进）。用户语义：点击画面停止快进 —— Ctrl 快进也一并
                // 停（若 keyup 因焦点/输入法丢失，点击是可靠的停止手段）。
                // 事件层已过滤 key repeat：物理仍按住时不会有新 ctrl down
                // 入队，清状态后不会立即重启；真正松开再按仍正常启动。
                if (m_skipMode || m_ctrlHeld)
                {
                    fprintf(stderr, "[ONS] -- click stops skip/ctrl-fastforward\n");
                    m_skipMode = false;
                    m_ctrlHeld = false;
                }
                OnsPushEvent(&ev); // 放回：等待分支正常处理（推进/按钮/rmenu）
                break;
            }
            OnsPushEvent(&ev);
            break;
        }
    }

    // automode/skip 自动推进：只自动过文本点击等待（上游 skip 只在文本模式
    // 自动；按钮/选择等待（btnwait/selectbtnwait/csel）必须用户输入 ——
    // skip 曾自动返回 0 越过 ever17 *customsel 选择）。Ctrl 按住 = 连点，
    // 可过文本与按钮等待（!m_btnwaitDelete 的 textbtnwait/selectbtnwait）。
    bool textAuto = (m_autoMode || m_skipMode || m_ctrlHeld) &&
                    m_wait == WM_TEXTCLICK;
    bool btnAuto = m_ctrlHeld && m_wait == WM_BTNWAIT && !m_btnwaitDelete;
    if (textAuto || btnAuto)
    {
        // 自动推进优先于等待分支的事件处理，会饿死真实输入 —— 先查事件：
        // 任何点击/按键 = 停止 automode/skip（上游点一下只停不推进）。
        OnsEvent ie;
        while (OnsPollEvent(&ie))
        {
            if (ie.type == ONS_EVENT_QUIT)
            {
                m_finished = true;
                return false;
            }
            if (ie.type == ONS_EVENT_MOUSEBUTTONDOWN || ie.type == ONS_EVENT_KEYDOWN)
            {
                if (m_autoMode || m_skipMode)
                {
                    m_autoMode = m_skipMode = false;
                    fprintf(stderr, "[ONS] -- automode/skip off (input)\n");
                }
            }
            OnsPushEvent(&ie); // 放回：由等待分支正常处理（该输入推进一次）
            break;
        }
        uint32_t now = OnsGetTicks();
        bool fire = (m_skipMode || m_ctrlHeld) || (now - m_waitStart >= (uint32_t)m_autoTime);
        if (fire && now >= m_waitStart)
        {
            m_waitStart = now;
            if (m_wait == WM_BTNWAIT)
            {
                SetVar(m_btnwaitVar, 0); // 上游 automode/skip：textbtnwait 返回 0
                HideBtnSprites();
                if (m_btnwaitDelete)
                    m_buttons.clear();
            }
            else if (m_textClickClear)
                ClearText();
            m_wait = WM_NONE;
            return true; // 本 tick 推进，下一 tick 继续执行脚本
        }
    }

    // 等待点处理
    if (m_wait == WM_CLICK)
    {
        OnsEvent ev;
        while (PollWaitEvent(&ev))
        {
            if (ev.type == ONS_EVENT_QUIT)
            {
                m_finished = true;
                return false;
            }
            if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN)
            {
                m_wait = WM_NONE;
                ClearText();
                break;
            }
            if (ev.type == ONS_EVENT_KEYDOWN)
            {
                int k = ev.u.key.keysym.sym;
                if (k == ONSK_SPACE || k == ONSK_RETURN || k == ONSK_ESCAPE)
                {
                    m_wait = WM_NONE;
                    ClearText();
                    break;
                }
            }
            // 其余事件（鼠标抬起/移动等）丢弃
        }
        if (m_wait == WM_CLICK)
            return true; // 继续等待
    }
    else if (m_wait == WM_SELECT)
    {
        OnsEvent ev;
        while (PollWaitEvent(&ev))
        {
            if (ev.type == ONS_EVENT_QUIT)
            {
                m_finished = true;
                return false;
            }
            if (ev.type == ONS_EVENT_KEYDOWN)
            {
                int k = ev.u.key.keysym.sym;
                if (k == ONSK_UP)
                {
                    if (!m_selectItems.empty())
                        m_selectCursor = (m_selectCursor + (int)m_selectItems.size() - 1) %
                                         (int)m_selectItems.size();
                    DrawSelectMenu();
                }
                else if (k == ONSK_DOWN)
                {
                    if (!m_selectItems.empty())
                        m_selectCursor = (m_selectCursor + 1) % (int)m_selectItems.size();
                    DrawSelectMenu();
                }
                else if (k == ONSK_RETURN || k == ONSK_SPACE)
                {
                    ConfirmSelect();
                    break;
                }
            }
            else if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN)
            {
                // 按菜单项行命中（屏幕坐标 = 文本层位置 + 层内菜单偏移）
                int idx = (ev.u.button.y - (m_textY + m_selectMenuY)) / m_lineSpacing;
                if (idx >= 0 && idx < (int)m_selectItems.size())
                {
                    m_selectCursor = idx;
                    ConfirmSelect();
                    break;
                }
            }
            // 其余事件（鼠标抬起等）丢弃
        }
        if (m_wait == WM_SELECT)
            return true; // 继续等待
    }
    else if (m_wait == WM_TEXTCLICK)
    {
        // 文本行点击推进（页断时清文本；textgosub 模式下由例程内等待接管）
        OnsEvent ev;
        while (PollWaitEvent(&ev))
        {
            if (ev.type == ONS_EVENT_QUIT)
            {
                m_finished = true;
                return false;
            }
            bool advance = false;
            if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN ||
                ev.type == ONS_EVENT_KEYDOWN)
            {
                // 任意真实输入先停 automode/skip（上游 mousePressEvent 的
                // automode_flag 分支：点一下只停不推进 —— 该输入被吞掉）
                if (m_autoMode || m_skipMode)
                {
                    m_autoMode = m_skipMode = false;
                    fprintf(stderr, "[ONS] -- automode/skip off (input)\n");
                    continue;
                }
            }
            if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN)
            {
                if (ev.u.button.button == 3)
                {
                    // 文本等待的右键 = rmenu（上游 WAIT_TEXT_MODE 右键 ->
                    // SYSTEM_MENU；有 *r_menu 标签交给脚本，否则内置菜单）
                    if (m_labels.find("r_menu") != m_labels.end())
                    {
                        m_wait = WM_NONE;
                        auto it = m_labels.find("r_menu");
                        m_pc = it->second - 1;
                    }
                    else
                    {
                        m_rmenuPrevWait = WM_TEXTCLICK; // 取消/动作后恢复
                        m_rmenuItems = m_rmenuDefItems; // 每次从主菜单唤起（防槽列表残留）
                        m_rmenuMainItems = m_rmenuDefItems;
                        m_rmenuSlotMode = false;
                        DrawRMenu();
                        m_wait = WM_RMENU;
                    }
                    break;
                }
                advance = true;
            }
            else if (ev.type == ONS_EVENT_KEYDOWN)
            {
                int k = ev.u.key.keysym.sym;
                if (k == ONSK_SPACE || k == ONSK_RETURN || k == ONSK_LCTRL || k == ONSK_RCTRL)
                    advance = true; // Ctrl：按住快进（上游 ctrl = 一次点击）
            }
            if (advance)
            {
                m_wait = WM_NONE;
                if (m_textClickClear)
                    ClearText();
                break;
            }
        }
        if (m_wait == WM_TEXTCLICK)
            return true; // 继续等待
    }
    else if (m_wait == WM_BTNWAIT)
    {
        // btnwait/textbtnwait：点击/按键 -> 按钮 id 或特殊值
        OnsEvent ev;
        while (PollWaitEvent(&ev))
        {
            if (ev.type == ONS_EVENT_QUIT)
            {
                m_finished = true;
                return false;
            }
            int v = -99;
            if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN || ev.type == ONS_EVENT_KEYDOWN)
            {
                // 真实输入先停 automode/skip（输入被吞；上游点一下只停不推进）
                if (m_autoMode || m_skipMode)
                {
                    m_autoMode = m_skipMode = false;
                    fprintf(stderr, "[ONS] -- automode/skip off (input)\n");
                    continue;
                }
            }
            if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN)
            {
                v = ClassifyButton(ev.u.button.button, ev.u.button.x, ev.u.button.y);
                if (v == -1 && !m_rmenuItems.empty())
                {
                    // 右键：优先交给脚本（CLANNAD 自带 *r_menu 图片按钮菜单）；
                    // 脚本没有 *r_menu 标签时用我们的兜底菜单
                    if (m_labels.find("r_menu") != m_labels.end())
                    {
                        // 返回 -1，脚本 goto *r_menu 处理
                    }
                    else
                    {
                        m_rmenuPrevWait = WM_BTNWAIT; // 取消/动作后恢复按钮等待
                        m_rmenuItems = m_rmenuDefItems; // 每次从主菜单唤起（防槽列表残留）
                        m_rmenuMainItems = m_rmenuDefItems;
                        m_rmenuSlotMode = false;
                        DrawRMenu();
                        m_wait = WM_RMENU;
                        break;
                    }
                }
            }
            else if (ev.type == ONS_EVENT_KEYDOWN)
            {
                int k = ev.u.key.keysym.sym;
                if (k == ONSK_ESCAPE)
                    v = -1; // 上游：ESC 等价右键（-1）
                else if (k == ONSK_LEFT)
                    v = -2; // ← = 滚轮上（-2）
                else if (k == ONSK_RIGHT)
                    v = -3; // → = 滚轮下（-3）
                else if (k == ONSK_SPACE || k == ONSK_RETURN)
                    v = 0; // 回车/空格 = 普通点击
                else if (k == ONSK_LCTRL || k == ONSK_RCTRL)
                    v = 0; // Ctrl = 推进
            }
            if (v == -99)
                continue;
            SetVar(m_btnwaitVar, v);
            m_wait = WM_NONE;
            // 等待结束：按钮不再显示（上游 show_flag=0，del_flag 只决定链删不删）
            HideBtnSprites();
            if (m_btnwaitDelete)
                m_buttons.clear();
            break;
        }
        if (m_wait == WM_BTNWAIT)
            return true; // 继续等待
    }
    else if (m_wait == WM_ERASE)
    {
        // windowerase（executeWindowErase）：文本框已隐藏，任意点击/按键恢复
        OnsEvent ev;
        while (PollWaitEvent(&ev))
        {
            if (ev.type == ONS_EVENT_QUIT)
            {
                m_finished = true;
                return false;
            }
            if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN || ev.type == ONS_EVENT_KEYDOWN)
            {
                if (m_screen)
                {
                    tTJSNI_BaseLayer* wl = m_screen->GetWindowLayer();
                    if (wl)
                    {
                        wl->SetVisible(true);
                        wl->Update();
                    }
                    tTJSNI_BaseLayer* tl = m_screen->GetTextLayer();
                    if (tl)
                    {
                        tl->SetVisible(true);
                        tl->Update();
                    }
                }
                m_erasePending = false;
                m_waitStart = OnsGetTicks();
                m_wait = m_erasePrevWait ? (WaitMode)m_erasePrevWait : WM_NONE;
                break;
            }
        }
        if (m_wait == WM_ERASE)
            return true; // 继续等待
    }
    else if (m_wait == WM_LOOKBACK)
    {
        // 回想回看（executeSystemLookback）：↑/↓ 翻页；左键/右键/ESC 退出
        OnsEvent ev;
        while (PollWaitEvent(&ev))
        {
            if (ev.type == ONS_EVENT_QUIT)
            {
                m_finished = true;
                return false;
            }
            if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN)
            {
                // 左/右键退出（上游空白点击停留；无滚轮时用 ↑↓ 浏览，
                // 左键退出更符合操作习惯；回看内容不可交互）
                ExitLookback();
                break;
            }
            else if (ev.type == ONS_EVENT_KEYDOWN)
            {
                int k = ev.u.key.keysym.sym;
                if (k == ONSK_ESCAPE)
                {
                    ExitLookback();
                    break;
                }
                else if (k == ONSK_UP)
                    LookbackPage(-1);
                else if (k == ONSK_DOWN)
                    LookbackPage(1);
            }
        }
        if (m_wait == WM_LOOKBACK)
            return true; // 继续回看
    }
    else if (m_wait == WM_RMENU)
    {
        // 右键菜单（对齐上游 executeSystemMenu 语义）：悬停高亮、左键项执行
        // 动作；右键/ESC：槽列表回主菜单、主菜单取消（恢复打开前等待）。
        OnsEvent ev;
        while (PollWaitEvent(&ev))
        {
            if (ev.type == ONS_EVENT_QUIT)
            {
                m_finished = true;
                return false;
            }
            if (ev.type == ONS_EVENT_MOUSEMOTION)
            {
                UpdateRMenuHover(ev.u.motion.x, ev.u.motion.y);
                continue;
            }
            if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN)
            {
                int x = ev.u.button.x, y = ev.u.button.y;
                // 命中检测
                int idx = -1;
                for (int i = 0; i < (int)m_rmenuItems.size(); i++)
                {
                    if (x >= m_rmenuRects[i * 4] &&
                        x < m_rmenuRects[i * 4] + m_rmenuRects[i * 4 + 2] &&
                        y >= m_rmenuRects[i * 4 + 1] &&
                        y < m_rmenuRects[i * 4 + 1] + m_rmenuRects[i * 4 + 3])
                    {
                        idx = i;
                        break;
                    }
                }
                bool isCancel = (ev.u.button.button == 3);
                if (!isCancel && idx < 0)
                    continue; // 左键点空白：重等（上游 while(button==0)）
                if (isCancel)
                {
                    if (m_rmenuSlotMode)
                    {
                        // 槽列表取消：回主菜单（对齐上游槽界面取消退出，
                        // 这里回主菜单更贴近 ever17 层级习惯）
                        m_rmenuItems = m_rmenuMainItems;
                        m_rmenuSlotMode = false;
                        DrawRMenu();
                        continue;
                    }
                    // 主菜单取消：恢复打开前等待
                    for (int i = 0; i < (int)m_rmenuItems.size(); i++)
                        m_screen->ClearSprite(RMENU_TEXT_BASE + i);
                    tTJSNI_BaseLayer* ml = m_screen ? m_screen->GetMenuLayer() : nullptr;
                    if (ml)
                    {
                        ml->SetVisible(false);
                        ml->Update();
                    }
                    m_rmenuVisible = false;
                    m_rmenuHover = -1;
                    m_rmenuRects.clear();
                    m_waitStart = OnsGetTicks();
                    m_wait = (WaitMode)m_rmenuPrevWait;
                    break;
                }
                int r = ExecRMenuAction(m_rmenuItems[idx].action);
                if (r == 2)
                {
                    m_rmenuHover = -1; // 已切子菜单：重绘后继续等
                    continue;
                }
                // 关闭菜单（动作完成；或读档/reset 立即继续脚本）
                for (int i = 0; i < (int)m_rmenuItems.size(); i++)
                    m_screen->ClearSprite(RMENU_TEXT_BASE + i);
                tTJSNI_BaseLayer* ml = m_screen ? m_screen->GetMenuLayer() : nullptr;
                if (ml)
                {
                    ml->SetVisible(false);
                    ml->Update();
                }
                m_rmenuVisible = false;
                m_rmenuHover = -1;
                m_rmenuRects.clear();
                if (r == 1)
                {
                    // 读档/reset/end：pc 已跳或流程已切换（windowerase 已置
                    // WM_ERASE、lookback 已置 WM_LOOKBACK）——动作未改等待态
                    // 时才直接继续脚本
                    if (m_wait == WM_RMENU)
                        m_wait = WM_NONE;
                }
                else
                {
                    m_waitStart = OnsGetTicks();
                    m_wait = (WaitMode)m_rmenuPrevWait; // 恢复打开前等待（文本/按钮等待）
                }
                break;
            }
            if (ev.type == ONS_EVENT_KEYDOWN)
            {
                int k = ev.u.key.keysym.sym;
                if (k == ONSK_ESCAPE)
                {
                    if (m_rmenuSlotMode)
                    {
                        m_rmenuItems = m_rmenuMainItems;
                        m_rmenuSlotMode = false;
                        DrawRMenu();
                        continue;
                    }
                    for (int i = 0; i < (int)m_rmenuItems.size(); i++)
                        m_screen->ClearSprite(RMENU_TEXT_BASE + i);
                    tTJSNI_BaseLayer* ml = m_screen ? m_screen->GetMenuLayer() : nullptr;
                    if (ml)
                    {
                        ml->SetVisible(false);
                        ml->Update();
                    }
                    m_rmenuVisible = false;
                    m_rmenuHover = -1;
                    m_rmenuRects.clear();
                    m_waitStart = OnsGetTicks();
                    m_wait = (WaitMode)m_rmenuPrevWait;
                    break;
                }
            }
        }
        if (m_wait == WM_RMENU)
            return true; // 继续等待
    }
    else if (m_wait == WM_MOVIE)
    {
        // 视频播放等待：click 参数时点击跳过；播完（IsDone）继续脚本
        if (m_movieClick)
        {
            OnsEvent ev;
            while (PollWaitEvent(&ev))
            {
                if (ev.type == ONS_EVENT_MOUSEBUTTONDOWN)
                {
                    if (m_movie)
                        m_movie->Stop();
                    m_wait = WM_NONE;
                    break;
                }
            }
        }
        if (m_wait == WM_MOVIE && m_movie && m_movie->IsDone())
        {
            m_movie->Stop();
            m_wait = WM_NONE;
        }
        if (m_wait == WM_MOVIE)
            return true; // 继续等待
    }
    else if (m_wait == WM_TIMER)
    {
        if (OnsGetTicks() < m_waitUntil)
            return true; // 继续等待
        m_wait = WM_NONE;
    }
    else if (m_wait == WM_EFFECT)
    {
        // 转场阻塞（上游 while(doEffect)：期间 OnsScreen::Tick 推进画面）
        if (OnsGetTicks() < m_waitUntil)
            return true; // 继续等待
        m_wait = WM_NONE;
    }

    // 执行直到等待点/结束（预算防止无等待死循环饿死主循环）
    int budget = 2000;
    while (m_wait == WM_NONE && !m_finished && budget-- > 0)
    {
        if (m_pc >= (int)m_lines.size())
        {
            m_finished = true;
            break;
        }
        try
        {
            if (!m_pendingTextSegs.empty())
            {
                // 裸 @ 停顿恢复：续画 @ 后的剩余段（@ 等待不清文本；接续同行）
                for (size_t k = 0; k < m_pendingTextSegs.size(); k++)
                {
                    bool append = (k > 0 && m_pendingAtWaits[k - 1]);
                    DrawTextLine(m_pendingTextSegs[k], append);
                    if (m_pendingAtWaits[k] && k + 1 < m_pendingTextSegs.size())
                    {
                        m_pendingTextSegs.erase(m_pendingTextSegs.begin(),
                                                m_pendingTextSegs.begin() + (long)k + 1);
                        m_pendingAtWaits.erase(m_pendingAtWaits.begin(),
                                               m_pendingAtWaits.begin() + (long)k + 1);
                        m_waitStart = OnsGetTicks();
                        m_wait = WM_TEXTCLICK;
                        m_textClickClear = false;
                        break;
                    }
                }
                if (m_wait == WM_TEXTCLICK)
                    break; // 又遇 @ 停顿
                m_pendingTextSegs.clear();
                m_pendingAtWaits.clear();
                // 本行全部段画完 -> 行尾处理（textgosub / 默认点击等待）
                if (!FinishTextLine(m_pendingTextEol))
                    break;
                continue;
            }
            if (!m_pendingStmts.empty())
            {
                // 行内等待恢复：继续执行本行剩余语句（wait 之后的 goto 等）
                if (!ExecStatementList(m_pendingStmts, m_pendingIdx))
                    break; // 又遇等待点
                // 本行全部语句完成：前进到下一行（若语句内 goto/gosub 已改
                // m_pc，+1 只是跳过目标标签行，无害）
                m_pc++;
                continue;
            }
            if (!ExecuteLine(m_pc))
            {
                if (m_pendingStmts.empty())
                {
                    // 整行等待（btnwait/click/wait 作为整行）：先前进到下一行再
                    // 让出，否则下次 Step 会重复执行同一行（等待点永不前进）
                    m_pc++;
                }
                break;
            }
        }
        catch (...)
        {
            // 单行执行异常：跳过该行继续（容错）
        }
        m_pc++;
    }
    return !m_finished;
}

bool OnsVM::IsKnownCommand(const std::string& cmd)
{
    static const char* knownCmds[] = {
        "abssetcursor", "add", "addkinsoku", "allsp2hide", "allsp2resume", "allsphide", "allspresume", "amsp",
        "amsp2", "arc", "atoi", "autoclick", "automode", "automode_time", "autosaveoff", "avi",
        "bar", "barclear", "bclear", "bcursor", "bdef", "bdown", "bexec", "bg",
        "bgcopy", "bgcpy", "bgm", "bgmfadein", "bgmfadeout", "bgmonce", "bgmstop", "bgmvol",
        "blt", "br", "break", "bsp", "btime", "btn", "btndef", "btndown",
        "btntime", "btntime2", "btnwait", "btnwait2", "btrans", "caption", "cell", "cellcheckexbtn",
        "cellcheckspbtn", "checkkey", "checkpage", "chvol", "cl", "click", "clickstr", "clickvoice",
        "cls", "cmp", "cos", "csel", "cselbtn", "cselgoto", "csp", "csp2",
        "date", "dec", "defaultfont", "defaultspeed", "definereset", "defmp3vol", "defsevol", "defsub",
        "defvoicevol", "delay", "deletescreenshot", "dim", "div", "draw", "drawbg", "drawbg2",
        "drawclear", "drawfill", "drawsp", "drawsp2", "drawsp3", "drawtext", "dwave", "dwaveload",
        "dwaveloop", "dwaveplay", "dwaveplayloop", "dwavestop", "effect", "effectblank", "effectcut", "effectskip",
        "else", "end", "endif", "english", "erasetextwindow", "exbtn", "exbtn_d", "exec_dll",
        "existspbtn", "fileexist", "filelog", "flushout", "for", "game", "getbgmvol", "getbtntimer",
        "getcselnum", "getcselstr", "getcursor", "getcursorpos", "getcursorpos2", "getenter", "getfunction", "getinsert",
        "getlog", "getlogtext", "getmclick", "getmouseover", "getmousepos", "getmp3vol", "getpage", "getpageup",
        "getparam", "getparam2", "getreg", "getret", "getsavestr", "getscreenshot", "getsevol", "getspmode",
        "getsppos", "getspsize", "gettab", "gettag", "gettaglog", "gettext", "gettimer", "getversion",
        "getvoicevol", "getzxc", "globalon", "gosub", "goto", "humanorder", "humanz", "if",
        "inc", "indent", "input", "intlimit", "isdown", "isfull", "ispage", "isskip",
        "itoa", "itoa2", "jump", "jumpb", "jumpf", "kidokumode", "kidokuskip", "kinsoku",
        "labellog", "layermessage", "ld", "len", "linepage", "linepage2", "load", "loadgame",
        "loadgosub", "locate", "logsp", "logsp2", "lookbackbutton", "lookbackcolor", "lookbackflush", "lookbacksp",
        "loopbgm", "loopbgmstop", "lr_trap", "lrclick", "ls", "lsp", "lsp2", "lsp2add",
        "lsp2sub", "lsph", "lsph2", "lsph2add", "lsph2sub", "luacall", "luasub", "maxkaisoupage",
        "menu_automode", "menu_click_def", "menu_click_page", "menu_full", "menu_window", "menuselectcolor", "menuselectvoice", "menusetwindow",
        "mid", "mod", "mode_ext", "mode_saya", "mode_wave_demo", "monocro", "mov", "mov10",
        "mov3", "mov4", "mov5", "mov6", "mov7", "mov8", "mov9", "movemousecursor",
        "movie", "movl", "mp3", "mp3fadein", "mp3fadeout", "mp3loop", "mp3save", "mp3stop",
        "mp3vol", "mpegplay", "msp", "msp2", "mul", "nega", "next", "nextcsel",
        "notif", "ns2", "ns3", "nsa", "nsadir", "numalias", "ofscopy", "ofscpy",
        "okcancelbox", "pagetag", "play", "playonce", "playstop", "playvoice", "pretextgosub", "print",
        "prnum", "prnumclear", "puttext", "quake", "quakex", "quakey", "quit", "repaint",
        "reset", "resettimer", "return", "rmenu", "rmode", "rnd", "rnd2", "roff",
        "rubyoff", "rubyon", "sar", "save", "savedir", "savefileexist", "savegame", "savegame2",
        "savename", "savenumber", "saveoff", "saveon", "savepoint", "savescreenshot", "savescreenshot2", "savetime",
        "se", "select", "selectbtnwait", "selectcolor", "selectvoice", "selgosub", "selnum", "set",
        "setcursor", "setkinsoku", "setlayer", "setwindow", "setwindow2", "setwindow3", "sevol", "shadedistance",
        "sin", "skip", "skipoff", "soundpressplgin", "sp_rgb_gradation", "spbtn", "spclclk", "spi",
        "split", "splitstring", "spreload", "spstr", "stop", "stralias", "strsp", "strsph",
        "sub", "systemcall", "tablegoto", "tal", "tan", "tateyoko", "texec", "textbtnwait",
        "textclear", "textcolor", "textgosub", "texthide", "textoff", "texton", "textshow", "textspeed",
        "textspeeddefault", "time", "transbtn", "transmode", "trap", "underline", "useescspc", "usewheel",
        "versionstr", "voicevol", "vsp", "vsp2", "wait", "waitclick", "waittimer", "wave",
        "waveloop", "wavestop", "windowback", "windowchip", "windoweffect", "yesnobox", "zenkakko", "zxc",
    };
    for (auto k : knownCmds)
        if (cmd == k)
            return true;
    return false;
}

bool OnsVM::ExecuteLine(int line)
{
    std::string raw = m_lines[line];
    std::string text = Trim(raw);

    // textgosub 页断延迟清文本：页断行 gosub 例程返回后的下一行执行前，
    // 清上一页文本（上游 newPage 清文本缓冲；窗背保持）
    if (m_pageBreakPending)
    {
        m_pageBreakPending = false;
        ClearText();
    }

    if (m_trace)
    {
        std::string t = text;
        if (t.size() > 110)
            t = t.substr(0, 110);
        fprintf(stderr, "[ONS] %d: %s\n", line, t.c_str());
    }
    if (text.empty())
        return true;
    if (text[0] == ';')
        return true;
    if (text[0] == '*')
        return true; // 标签：顺序执行自然经过

    // 命令还是文本？（'`' 前缀强制文本）
    bool forceText = text[0] == '`';
    if (forceText)
        text = text.substr(1);
    size_t nameEnd = 0;
    while (nameEnd < text.size() &&
           ((text[nameEnd] >= 'a' && text[nameEnd] <= 'z') ||
            (text[nameEnd] >= 'A' && text[nameEnd] <= 'Z') ||
            (text[nameEnd] >= '0' && text[nameEnd] <= '9') || text[nameEnd] == '_'))
        nameEnd++;

    if (!forceText && nameEnd > 0 && IsKnownCommand(ToLower(text.substr(0, nameEnd))))
    {
        // if/notif：行内命令延伸到整行结尾（含 ':' 语句），整行作为一个语句
        // （NScr：if 假时跳过整行；真时执行表达式后的全部命令）
        std::string first = ToLower(text.substr(0, nameEnd));
        if (first == "if" || first == "notif")
            return ExecuteStatement(text);
        // 命令行：按 ':' 拆分为多条语句依次执行；遇等待/结束则暂停本行，
        // 等待结束后从剩余语句继续（ExecStatementList）
        std::vector<std::string> stmts;
        SplitStatements(text, stmts);
        return ExecStatementList(stmts, 0);
    }
    // 未知命令：形如 "单词 参数" 的行按命令跳过（NScr 中命令行就是命令；
    // 避免未实现的命令被当文本画出来）。纯文本请用 '`' 前缀强制。
    if (!forceText && nameEnd > 0 &&
        (text.size() == nameEnd ||
         text[nameEnd] == ' ' || text[nameEnd] == '\t' || text[nameEnd] == ','))
    {
        // defsub 子例程名单独一行 = gosub（上游 parseLine user_func_hash）
        std::string name = ToLower(text.substr(0, nameEnd));
        if (m_defsubs.count(name))
        {
            auto it = m_labels.find(name);
            if (it != m_labels.end())
            {
                // 调用行剩余部分 = 子例程参数（getparam 读取）
                std::vector<std::string> params;
                std::string rest = Trim(text.substr(nameEnd));
                if (!rest.empty())
                {
                    SplitArgs(rest, params);
                    for (auto& p : params)
                        p = Unquote(Trim(p));
                }
                m_subParams.push_back(params);
                m_callStack.push_back(m_pc);
                m_pc = it->second - 1;
                return true;
            }
        }
        // 诊断：未知命令
        fprintf(stderr, "[ONS] -- UNKNOWN cmd='%s' (line %d): %s\n", name.c_str(), line,
                text.substr(0, 80).c_str());
        return true;
    }

    // 文本行（含 \ 标签 / EOL / 页断 / textgosub 处理）
    // pretextgosub（上游 ONScripter_text.cpp processText 行首 [tag] 处理）：
    // 行首 [tag] 在文本显示前剥离并 gosub 例程（例程内 gettag 读 tag，
    // 如 ever17 的 [NONE|1]/[eop008|1] 说话人标记）；例程 return 后本行
    // 正文才显示。无 pretextgosub 注册时 [..] 保持为普通文本（CLANNAD 不受影响）。
    if (m_pretextConsumed)
    {
        text = m_pretextBody;
        m_pretextBody.clear();
        m_pretextConsumed = false;
    }
    else if (!m_pretextLabel.empty() && !text.empty() && text[0] == '[')
    {
        size_t rb = text.find(']');
        if (rb != std::string::npos)
        {
            m_currentTag = text.substr(1, rb - 1);
            m_pretextBody = text.substr(rb + 1);
            auto it = m_labels.find(m_pretextLabel);
            if (it != m_labels.end())
            {
                m_pretextConsumed = true;
                m_pretextReturnLine = m_pc;
                m_callStack.push_back(PRETEXT_RET_MARK);
                m_pc = it->second - 1; // Step +1 -> 例程首行
            }
            else
                text = m_pretextBody; // 标签缺失：剥离 tag 直接显示正文
            return true;
        }
    }
    return DrawTextPage(text);
}

// 语句列表执行（NScr 行内链）：逐条执行；某条遇到等待点（返回 false）时把
// 剩余语句存入 m_pendingStmts 并返回 false；等待结束后 Step 调用本函数续跑。
void OnsVM::ExpandCommaStatements(const std::string& stmt, std::vector<std::string>& out)
{
    // NScr 词法允许逗号分隔语句（命令参数读完后，逗号后跟命令名即新语句），
    // 例：CLANNAD 'itoa2 $3000,%3000,itoa2 $4000,%4000'。
    // 引号外逗号后紧跟已知命令名 -> 在此截断（前段成句，剩余递归继续）。
    bool inq = false;
    for (size_t i = 0; i < stmt.size(); i++)
    {
        if (stmt[i] == '"')
        {
            inq = !inq;
            continue;
        }
        if (inq || stmt[i] != ',')
            continue;
        size_t j = i + 1;
        while (j < stmt.size() && (stmt[j] == ' ' || stmt[j] == '\t'))
            j++;
        size_t k = j;
        while (k < stmt.size() && ((stmt[k] >= 'a' && stmt[k] <= 'z') ||
                                   (stmt[k] >= 'A' && stmt[k] <= 'Z') ||
                                   (stmt[k] >= '0' && stmt[k] <= '9') || stmt[k] == '_'))
            k++;
        if (k > j && IsKnownCommand(ToLower(stmt.substr(j, k - j))))
        {
            out.push_back(Trim(stmt.substr(0, i))); // 前段不含逗号
            ExpandCommaStatements(stmt.substr(j), out);
            return;
        }
    }
    out.push_back(Trim(stmt));
}

bool OnsVM::ExecStatementList(const std::vector<std::string>& stmts, size_t startIdx)
{
    // 先展开逗号粘连语句（展开结果同样受等待/续跑管理）
    std::vector<std::string> expanded;
    for (const auto& st : stmts)
        ExpandCommaStatements(st, expanded);
    for (size_t i = startIdx; i < expanded.size(); i++)
    {
        if (!ExecuteStatement(expanded[i]))
        {
            m_pendingStmts = expanded;
            m_pendingIdx = i + 1;
            return false;
        }
    }
    m_pendingStmts.clear();
    m_pendingIdx = 0;
    return true;
}

bool OnsVM::ExecuteStatement(const std::string& stmt)
{
    std::string text = Trim(StripInlineComment(stmt));
    if (text.empty())
        return true;

    size_t nameEnd = 0;
    while (nameEnd < text.size() &&
           ((text[nameEnd] >= 'a' && text[nameEnd] <= 'z') ||
            (text[nameEnd] >= 'A' && text[nameEnd] <= 'Z') ||
            (text[nameEnd] >= '0' && text[nameEnd] <= '9') || text[nameEnd] == '_'))
        nameEnd++;
    if (nameEnd == 0)
        return true; // 非命令（不应到达）
    std::string cmd = ToLower(text.substr(0, nameEnd));
    std::string args = Trim(text.substr(nameEnd));
    // 去掉开头的空白/逗号
    if (!args.empty() && (args[0] == ' ' || args[0] == '\t' || args[0] == ','))
        args = Trim(args.substr(1));

    if (cmd == "if" || cmd == "notif")
        return ExecuteIf(args, cmd == "notif");
    if (cmd == "for")
        return ExecuteFor(args);
    if (cmd == "next")
        return ExecuteNext();
    if (cmd == "textgosub")
    {
        // DEFINE 段设置：行尾点击后 gosub 的标签（LoadDefineSection 也收集）
        if (!args.empty() && args[0] == '*')
            m_textGosubLabel = args.substr(1);
        return true;
    }
    if (cmd == "pretextgosub")
    {
        // DEFINE 段设置：行首 [tag] 文本显示前 gosub 的标签
        // （上游 pretextgosubCommand：pretextgosub_label；例程内 gettag 读 tag）
        if (!args.empty() && args[0] == '*')
            m_pretextLabel = args.substr(1);
        return true;
    }
    if (cmd == "gettag")
    {
        // 例程内取当前行 [tag]（上游 gettagCommand：current_page->tag ->
        // $变量；仅在 pretextgosub 例程中有效）
        std::string v = Unquote(Trim(args));
        if (!v.empty() && v[0] == '$')
            v = v.substr(1);
        if (!v.empty())
            SetStrVar(v, m_currentTag);
        return true;
    }
    // 矩形色块参数（>w,h,#color）整体保留，不按逗号拆分
    if ((cmd == "bg" || cmd == "bgcopy") && !args.empty() && args[0] == '>')
    {
        std::vector<std::string> a;
        a.push_back(args);
        return ExecuteCommand(cmd, a);
    }
    // select/selnum/csel：菜单项为带引号字符串（内含逗号），保留原始参数解析
    if (cmd == "select" || cmd == "selnum" || cmd == "csel")
    {
        std::vector<std::string> a;
        a.push_back(args);
        return ExecuteCommand(cmd, a);
    }
    std::vector<std::string> a;
    SplitArgs(args, a);
    return ExecuteCommand(cmd, a);
}

bool OnsVM::ExecuteIf(const std::string& args, bool negate)
{
    // NScr：if <expr> [行内命令...]；行内命令延伸到整行结尾（可含 ':' 语句），
    // 假时跳过整行；无行内命令时为块 if（跳过到 else/endif）。
    ExprParser p(args, this);
    int v = 0;
    bool ok = p.ParseExpr(v);
    if (!ok)
        v = 0;
    if (negate)
        v = v ? 0 : 1;

    std::string rest = Trim(args.substr(p.EndPos()));
    if (v != 0)
    {
        if (!rest.empty())
        {
            // 行内命令：可能含 ':' 语句，逐条执行；遇等待/结束暂停，
            // 等待结束后从剩余语句继续（ExecStatementList）
            std::vector<std::string> stmts;
            SplitStatements(rest, stmts);
            return ExecStatementList(stmts, 0);
        }
        return true; // 块 if 真分支：继续下一行
    }
    if (!rest.empty())
        return true; // 行内 if 假：仅跳过本语句（整行）
    // 块 if 假：跳过直到匹配的 else / endif（支持嵌套计数）
    int depth = 1;
    for (int i = m_pc + 1; i < (int)m_lines.size(); i++)
    {
        std::string t = ToLower(Trim(m_lines[i]));
        if (t == "if" || t == "notif")
            depth++;
        else if (t == "endif")
        {
            depth--;
            if (depth == 0)
            {
                m_pc = i;
                break;
            }
        }
        else if (t == "else" && depth == 1)
        {
            m_pc = i; // 执行 else 分支
            break;
        }
    }
    return true;
}

bool OnsVM::ExecuteFor(const std::string& args)
{
    // for %v=start to end [step s]
    size_t eq = args.find('=');
    if (eq == std::string::npos)
        return true;
    std::string var = Trim(args.substr(0, eq));
    if (!var.empty() && var[0] == '%')
        var = var.substr(1);
    std::string rest = args.substr(eq + 1);
    size_t toPos = rest.find(" to ");
    std::string startS, endS, stepS;
    if (toPos == std::string::npos)
    {
        // "to" 可能前后无空格
        size_t p = rest.find("to");
        if (p == std::string::npos)
            return true;
        startS = Trim(rest.substr(0, p));
        endS = Trim(rest.substr(p + 2));
    }
    else
    {
        startS = Trim(rest.substr(0, toPos));
        endS = Trim(rest.substr(toPos + 4));
    }
    size_t stepPos = endS.find(" step ");
    if (stepPos != std::string::npos)
    {
        stepS = Trim(endS.substr(stepPos + 6));
        endS = Trim(endS.substr(0, stepPos));
    }
    int start = 0, end = 0, step = 1;
    EvalExpression(startS, start);
    EvalExpression(endS, end);
    if (!stepS.empty())
        EvalExpression(stepS, step);
    SetVar(var, start);
    LoopFrame f;
    f.var = var;
    f.endVal = end;
    f.step = step;
    f.bodyLine = m_pc + 1; // 循环体起始（for 的下一行；Step 循环会 +1，这里 -1 补偿）
    m_loops.push_back(f);
    return true;
}

bool OnsVM::ExecuteNext()
{
    if (m_loops.empty())
        return true;
    LoopFrame& f = m_loops.back();
    int v = 0;
    GetVar(f.var, v);
    v += f.step;
    SetVar(f.var, v);
    bool cont = f.step > 0 ? (v <= f.endVal) : (v >= f.endVal);
    if (cont)
        m_pc = f.bodyLine - 1; // Step 循环会 +1
    else
        m_loops.pop_back();
    return true;
}

// 解析指令尾随特效参数（上游 ScriptParser::parseEffect/readEffect 语义）：
// - 无参数：effect 0（即时，setEffect 里 effect==0 直接返回）；
// - 单参数 e：e==0/1 即时；否则查 DEFINE 段特效注册表（m_effects，键=e）；
// - e,duration：行内特效（e 为特效编号，duration 毫秒）。
// 未注册的编号：上游 errorAndExit；本项目记录日志并按即时处理（容错）。
std::pair<int, int> OnsVM::ResolveEffect(const std::vector<std::string>& args, size_t idx)
{
    if (idx >= args.size() || args[idx].empty())
        return std::make_pair(0, 0);
    int e = 0;
    if (!EvalExpression(args[idx], e))
        return std::make_pair(0, 0);
    if (e == 0 || e == 1)
        return std::make_pair(e, 0);
    int dur = 0;
    if (idx + 1 < args.size() && !args[idx + 1].empty())
        EvalExpression(args[idx + 1], dur);
    if (dur > 0)
        return std::make_pair(e, dur); // 行内特效
    auto it = m_effects.find(e);
    if (it != m_effects.end())
        return it->second; // 注册表：effect 2,10,500 -> 键 2 = (10, 500)
    fprintf(stderr, "[ONS] -- effect No. %d is not registered (instant)\n", e);
    return std::make_pair(0, 0);
}

bool OnsVM::ExecuteCommand(const std::string& cmd, const std::vector<std::string>& args)
{
    if (cmd == "exbtn_d" || cmd == "exbtn")
        fprintf(stderr, "[ONS] -- EC cmd=%s args=%zu\n", cmd.c_str(), args.size());
    auto arg = [&](size_t i) -> std::string {
        return i < args.size() ? Unquote(args[i]) : "";
    };

    // 标签查找（参数可能带 '*' 前缀）
    auto findLabel = [&](const std::string& s) {
        std::string name = s;
        if (!name.empty() && name[0] == '*')
            name = name.substr(1);
        return m_labels.find(ToLower(name));
    };

    if (cmd == "jump" || cmd == "goto")
    {
        auto it = findLabel(arg(0));
        if (it != m_labels.end())
        {
            // goto *start = 回标题画面重置（CLANNAD rmenu"返回标题"即
            // goto *start，403205）。rmenu 打开时脚本 erasetextwindow 0
            // 使 LeaveTextDisplayMode(false) 不擦窗 -> 回标题后对话框残留；
            // 这里按画面重置强制清文本窗并复位快进/擦除等待状态。
            if (it->first == "start")
                ResetToStart();
            m_pc = it->second - 1; // Step 循环会 +1
        }
        return true;
    }
    if (cmd == "gosub")
    {
        auto it = findLabel(arg(0));
        if (it != m_labels.end())
        {
            // 调用行剩余参数（getparam 读取；上游 last_nest_info->next_script）
            std::vector<std::string> params;
            for (size_t i = 1; i < args.size(); i++)
            {
                std::string a = Trim(Unquote(args[i]));
                if (!a.empty())
                    params.push_back(a);
            }
            m_subParams.push_back(params);
            m_callStack.push_back(m_pc);
            m_pc = it->second - 1;
        }
        return true;
    }
    if (cmd == "return")
    {
        if (!m_subParams.empty())
            m_subParams.pop_back();
        if (!m_callStack.empty())
        {
            int ret = m_callStack.back();
            m_callStack.pop_back();
            if (ret == PRETEXT_RET_MARK)
            {
                // pretext 例程返回：回到文本行本身（Step 的 +1 前停在
                // m_pretextReturnLine-1），pretextConsumed 分支显示正文
                m_pc = m_pretextReturnLine - 1;
                m_pretextReturnLine = -1;
            }
            else
                m_pc = ret;
        }
        return true;
    }
    if (cmd == "if")
    {
        // 已在 ExecuteLine 中处理（需要原始表达式文本）
        return true;
    }
    if (cmd == "else" || cmd == "endif")
    {
        // 单独出现（异常脚本）：跳过
        return true;
    }
    if (cmd == "mov" || cmd == "set")
    {
        if (args.size() >= 2)
        {
            int v = 0;
            if (EvalExpression(arg(1), v))
                SetVar(arg(0), v);
        }
        return true;
    }
    if (cmd == "add" || cmd == "sub" || cmd == "mul" || cmd == "div")
    {
        // add/sub/mul/div（上游 ScriptParser_command.cpp addCommand 等）：
        // %v 与右侧表达式运算写回；add 对 $ 串为字符串拼接（$str += 内容）
        std::string dst = args.empty() ? "" : args[0];
        if (cmd == "add" && !dst.empty() && dst[0] == '$')
        {
            std::string name = dst.substr(1);
            std::string cur;
            GetStrVar(name, cur);
            cur += EvalStringExpr(GetRawArg(args, 1));
            SetStrVar(name, cur);
            return true;
        }
        if (dst.empty())
            return true;
        int val = 0, rhs = 0;
        GetVar(dst, val);
        if (args.size() >= 2 && !arg(1).empty())
            EvalExpression(arg(1), rhs);
        if (cmd == "add")
            val += rhs;
        else if (cmd == "sub")
            val -= rhs;
        else if (cmd == "mul")
            val *= rhs;
        else
            val = rhs ? val / rhs : 0;
        SetVar(dst, val);
        return true;
    }
    if (cmd == "cell")
    {
        // cell sprite_no,no（上游 cellCommand）：精灵切帧
        int n = 0, c = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), n);
        if (!arg(1).empty())
            EvalExpression(arg(1), c);
        if (m_screen)
            m_screen->SetSpriteCell(n, c);
        return true;
    }
    if (cmd == "inc" || cmd == "dec")
    {
        int v = 0;
        GetVar(arg(0), v);
        int delta = 1;
        if (!arg(1).empty())
            EvalExpression(arg(1), delta);
        SetVar(arg(0), v + (cmd == "inc" ? delta : -delta));
        return true;
    }
    if (cmd == "dim")
    {
        if (args.size() >= 2)
        {
            int n = 0;
            if (EvalExpression(arg(1), n) && n > 0 && n < 100000)
                m_arrays[arg(0)] = std::vector<int>((size_t)n, 0);
        }
        return true;
    }
    if (cmd == "end" || cmd == "quit")
    {
        m_finished = true;
        return false;
    }
    if (cmd == "click" || cmd == "waitclick")
    {
        WaitForClick();
        return false;
    }
    if (cmd == "wait" || cmd == "delay")
    {
        int ms = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), ms);
        WaitForTicks((uint32_t)std::max(0, ms));
        return false;
    }
    if (cmd == "textclear")
    {
        ClearText();
        return true;
    }
    if (cmd == "cl")
    {
        // cl [a|l|c|r]：清除立绘（tachi；a=全部，缺省=全部）
        char loc = 'a';
        if (!arg(0).empty())
            loc = (char)ToLower(arg(0))[0];
        if (!m_screen)
            return true;
        if (loc == 'a' || loc == 'l')
            m_screen->ClearSprite(TACHI_BASE + 0);
        if (loc == 'a' || loc == 'c')
            m_screen->ClearSprite(TACHI_BASE + 1);
        if (loc == 'a' || loc == 'r')
            m_screen->ClearSprite(TACHI_BASE + 2);
        return true;
    }
    if (cmd == "cls")
    {
        ClearText();
        if (m_screen)
            m_screen->ClearSprite(-1); // 清屏：含全部精灵
        return true;
    }
    if (cmd == "setwindow" || cmd == "setwindow3")
    {
        // 上游 setwindowCore（setwindow3 = setwindowCore + 清页）：
        //   setwindow top_x,top_y,cols,rows,font_x,font_y,pitch_x_extra,
        //   setwindow top_x,top_y,cols,rows,font_x,font_y,pitch_x_extra,
        //             pitch_y_extra,wait_time,bold,shadow,"窗口背景图",x1,y1[,x2,y2]
        //   窗口像素尺寸 = cols*pitch_x x rows*pitch_y；
        //   背景图（sentence_font_info）blit 在 (x1,y1)（可 #RRGGBB 纯色模式）。
        auto toInt = [&](const std::string& s, int dflt) {
            int v = dflt;
            EvalExpression(s, v);
            return v;
        };
        m_textX = toInt(arg(0), 0);
        m_textY = toInt(arg(1), 0);
        int cols = toInt(arg(2), 20);
        int rows = toInt(arg(3), 4);
        int fontX = std::max(1, toInt(arg(4), 24));
        int fontY = std::max(1, toInt(arg(5), fontX));
        int pitchXExtra = toInt(arg(6), 0);
        int pitchYExtra = toInt(arg(7), 0);
        m_fontSize = fontY;
        m_lineSpacing = std::max(1, fontY + pitchYExtra);
        m_textShadow = toInt(arg(10), 0) != 0; // 第 11 参数 is_shadow
        m_textW = cols * (fontX + pitchXExtra);
        m_textH = rows * m_lineSpacing;
        if (m_textW <= 0)
            m_textW = m_screenW;
        if (m_textH <= 0)
            m_textH = 120;
        // 窗口背景（第 12 参数起：图像或 #颜色 + 位置）
        m_windowImage.clear();
        m_windowColor = 0xffffff;
        m_windowColorMode = false;
        m_winX = m_textX;
        m_winY = m_textY;
        if (args.size() >= 12)
        {
            std::string w = arg(11);
            if (!w.empty() && w[0] == '#')
            {
                m_windowColorMode = true;
                uint32_t c = 0;
                for (size_t k = 1; k < w.size() && k <= 6; k++)
                {
                    char hc = w[k];
                    c <<= 4;
                    if (hc >= '0' && hc <= '9')
                        c |= (uint32_t)(hc - '0');
                    else if (hc >= 'a' && hc <= 'f')
                        c |= (uint32_t)(hc - 'a' + 10);
                    else if (hc >= 'A' && hc <= 'F')
                        c |= (uint32_t)(hc - 'A' + 10);
                }
                m_windowColor = c | 0xff000000;
            }
            else
            {
                m_windowImage = w;
            }
            m_winX = toInt(arg(12), m_textX);
            m_winY = toInt(arg(13), m_textY);
        }
        ApplyTextWindow();
        fprintf(stderr, "[ONS] -- setwindow tx=%d ty=%d tw=%d th=%d win='%s' winx=%d winy=%d\n",
                m_textX, m_textY, m_textW, m_textH, m_windowImage.c_str(), m_winX, m_winY);
        m_textCursorX = 0;
        m_textCursorY = 0;
        return true;
    }
    if (cmd == "bg")
    {
        std::string file = arg(0);
        if (!m_screen)
            return true;
        // 上游 bgCommand 先 leaveTextDisplayMode（文本窗随 bg 切换消失）。
        // 顺序：先快照旧整帧（含当前窗背文字；上游 effect_src 即旧整帧），
        // 再 leave/清立绘/载入新 bg —— 转场淡出显示的是完整旧画面。
        tTJSNI_BaseLayer* bg = m_screen->GetPrimaryLayer();
        if (!bg)
            return true;
        // 存档记录当前 bg 规格（读档重放）。用 EvalStringExpr 展开后的值：
        // 动态拼接 bg（"background\"+$N+\".jpg"）若不展开，读档重放时 $N 已变/为空，
        // 文件名 resolve 失败、背景恢复不了（ever17 读档背景丢失根因之一）
        m_bgSpec = EvalStringExpr(GetRawArg(args, 0));
        // 尾随特效参数（上游 bgCommand parseEffect(true) + setEffect/doEffect）：
        // 先快照旧画面再载入新 bg（快照层盖在新画面上淡出 = crossfade）。
        std::pair<int, int> fx = ResolveEffect(args, 1);
        bool fxBlocking = false;
        if (fx.first > 1 && fx.second > 0)
            fxBlocking = m_screen->StartBgTransition(fx.first, fx.second);
        LeaveTextDisplayMode(false);
        // 上游 bgCommand：清除全部立绘
        m_screen->ClearSprite(TACHI_BASE + 0);
        m_screen->ClearSprite(TACHI_BASE + 1);
        m_screen->ClearSprite(TACHI_BASE + 2);
        // >w,h,#color... 矩形色块
        int w, h;
        std::vector<uint32_t> colors;
        if (ParseRectSpec(file, w, h, colors))
        {
            int n = (int)colors.size();
            for (int i = 0; i < n; i++)
            {
                int x0 = w * i / n, x1 = w * (i + 1) / n;
                if (i == n - 1)
                    x1 = w;
                bg->FillRect(tTVPRect(x0, 0, x1, h), colors[i]);
            }
            bg->Update(); // 标脏刷新（否则合成不重绘）
            if (fxBlocking)
            {
                m_wait = WM_EFFECT;
                m_waitUntil = OnsGetTicks() + (uint32_t)fx.second;
                return false; // 阻塞至转场结束（上游 while(doEffect)）
            }
            return true;
        }
        // NScr 颜色名：bg white / bg black
        if (file == "white")
        {
            bg->FillRect(tTVPRect(0, 0, m_screenW, m_screenH), 0xffffff);
            bg->Update();
            if (fxBlocking)
            {
                m_wait = WM_EFFECT;
                m_waitUntil = OnsGetTicks() + (uint32_t)fx.second;
                return false;
            }
            return true;
        }
        if (file == "black")
        {
            bg->FillRect(tTVPRect(0, 0, m_screenW, m_screenH), 0x000000);
            bg->Update();
            if (fxBlocking)
            {
                m_wait = WM_EFFECT;
                m_waitUntil = OnsGetTicks() + (uint32_t)fx.second;
                return false;
            }
            return true;
        }
        // 图片：经 krkrsdl3 存储加载（磁盘优先，失败试 >arc.nsa> 封包）；clNone 不挖透明。
        // 载入隐藏源层后缩放铺满主层 —— 主层尺寸恒定（LoadImages 直载主层会按图
        // 缩小主层：InternalSetImageSize，图小于窗口时画面只剩图片区域"一小撮"）。
        try
        {
            std::string bgf = OnsResolveStorageName(EvalStringExpr(GetRawArg(args, 0)));
            fprintf(stderr, "[ONS] -- bg '%s' -> '%s'\n", args[0].c_str(), bgf.c_str());
            if (!bgf.empty())
            {
                if (!m_screen->LoadBgScaled(bgf))
                    fprintf(stderr, "[ONS] -- bg load FAILED '%s'\n", bgf.c_str());
            }
        }
        catch (...)
        {
        }
        if (fxBlocking)
        {
            m_wait = WM_EFFECT;
            m_waitUntil = OnsGetTicks() + (uint32_t)fx.second;
            return false; // 阻塞至转场结束
        }
        return true;
    }
    if (cmd == "blt")
    {
        // 上游 bltCommand（command.cpp:3665）：从 btndef 图源（btndef_info，
        // 本项目 BTN_SRC_LAYER=250 精灵层）裁剪 (sx,sy,sw,sh)，缩放贴到主层
        // (dx,dy,dw,dh)。ever17 specialEffectImage/procPreGo 用它贴背景大图
        // （btndef "background\BG41A1.jpg" + blt 全屏）与缩放动画。
        auto toInt = [&](const std::string& s, int dflt) {
            int v = dflt;
            EvalExpression(s, v);
            return v;
        };
        int dx = toInt(arg(0), 0), dy = toInt(arg(1), 0);
        int dw = toInt(arg(2), 0), dh = toInt(arg(3), 0);
        int sx = toInt(arg(4), 0), sy = toInt(arg(5), 0);
        int sw = toInt(arg(6), 0), sh = toInt(arg(7), 0);
        if (!m_screen || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
            return true;
        tTJSNI_BaseLayer* src = m_screen->GetSpriteLayer(OnsScreen::BTN_SRC_LAYER);
        tTJSNI_BaseLayer* dst = m_screen->GetPrimaryLayer();
        if (!src || !dst)
            return true;
        try
        {
            tTVPBaseTexture* simg = src->GetMainImage();
            if (!simg)
                return true;
            int sw2 = (int)simg->GetWidth(), sh2 = (int)simg->GetHeight();
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            if (sx + sw > sw2) sw = sw2 - sx;
            if (sy + sh > sh2) sh = sh2 - sy;
            if (sw <= 0 || sh <= 0)
                return true;
            void* dbuf = dst->GetMainImagePixelBufferForWrite();
            int dpitch = dst->GetMainImagePixelBufferPitch();
            int dw2 = (int)dst->GetWidth(), dh2 = (int)dst->GetHeight();
            if (!dbuf || dpitch <= 0)
                return true;
            fprintf(stderr, "[ONS] -- blt dst=(%d,%d %dx%d) src=(%d,%d %dx%d) img=%dx%d\n",
                    dx, dy, dw, dh, sx, sy, sw, sh, sw2, sh2);
            for (int y = 0; y < dh; y++)
            {
                int ty = dy + y;
                if (ty < 0 || ty >= dh2)
                    continue;
                int srowy = sy + y * sh / dh;
                const tjs_uint8* srow =
                    (const tjs_uint8*)simg->GetScanLine(srowy);
                tjs_uint8* drow = (tjs_uint8*)dbuf + (size_t)ty * dpitch;
                for (int x = 0; x < dw; x++)
                {
                    int tx = dx + x;
                    if (tx < 0 || tx >= dw2)
                        continue;
                    int scol = sx + x * sw / dw;
                    std::memcpy(drow + (size_t)tx * 4, srow + (size_t)scol * 4, 4);
                }
            }
            dst->Update(); // CPU 直写：手动标脏
            return true;
        }
        catch (...)
        {
            return true;
        }
    }
    // ---- 精灵：ld/ls/lsp/amsp/csp（转发到 OnsScreen 精灵层组）----
    // ---- 精灵：lsp/lsph/lsp2/amsp/msp/vsp/csp/ld（真实 NScr 语义）----
    if (cmd == "lsp" || cmd == "lsph" || cmd == "lsp2" || cmd == "amsp" || cmd == "msp" ||
        cmd == "vsp" || cmd == "csp")
    {
        if (!m_screen)
            return true;
        auto toInt = [&](const std::string& s, int dflt) {
            int v = dflt;
            EvalExpression(s, v);
            return v;
        };
        int n = toInt(arg(0), 0);
        if (cmd == "csp")
        {
            m_screen->ClearSprite(n < 0 ? -1 : n); // n<0 全部
            if (n < 0)
                m_spriteState.clear();
            else
                m_spriteState.erase(n);
            return true;
        }
        if (cmd == "vsp")
        {
            // vsp n,visible（上游 vspCommand 先 leaveTextDisplayMode；窗背画在
            // 文本层 —— 下一条文本行 DrawTextPage 自动重画窗背+清旧文字）
            LeaveTextDisplayMode(false);
            int vis = toInt(arg(1), 0);
            m_screen->SetSpriteVisible(n, vis != 0);
            auto it = m_spriteState.find(n);
            if (it != m_spriteState.end())
                it->second.visible = (vis != 0);
            return true;
        }
        if (cmd == "lsp" || cmd == "lsph" || cmd == "lsp2")
        {
            // lsp n,file,x,y[,alpha]；lsph 载入但不显示；lsp2 同上（缩放未实现）
            // file 先整体求值（EvalStringExpr：剥引号 + "..."+$var+... 拼接；
            // 上游 lspCommand 经 parseTaggedString 同样先处理整串）
            std::string file = EvalStringExpr(GetRawArg(args, 1));
            int x = toInt(arg(2), 0);
            int y = toInt(arg(3), 0);
            int alpha = -1;
            if (!arg(4).empty())
                alpha = toInt(arg(4), -1);
            if (!file.empty())
            {
                SpriteState st;
                st.x = x;
                st.y = y;
                st.alpha = alpha;
                st.visible = (cmd != "lsph");
                // :s/ 字符串精灵（NScr TRANS_STRING，上游 parseTaggedString）：
                //   :s/字号x,字号y[,间距增量][,aa];#RRGGBB#RRGGBB...文本
                // 分号后连续的 #RRGGBB 是颜色列表 = 帧（每色一帧；CLANNAD 选项
                // 文字 #ffffff 普通 / #CCCCCC 悬停高亮），颜色列表之后才是文本。
                if (file.size() >= 3 && file[0] == ':' && file[1] == 's' && file[2] == '/')
                {
                    std::string spec = file.substr(3);
                    int fs = 18;
                    std::vector<uint32_t> colors;
                    std::string text;
                    // 字号段（字号x,字号y[,间距增量][,aa]；数字逗号分隔，全部跳过）
                    size_t p1 = spec.find(',');
                    if (p1 != std::string::npos)
                    {
                        fs = atoi(spec.substr(0, p1).c_str());
                        size_t p2 = spec.find(',', p1 + 1);
                        if (p2 != std::string::npos)
                        {
                            size_t p3 = spec.find(',', p2 + 1);
                            if (p3 != std::string::npos)
                                spec = spec.substr(p3 + 1);
                            else
                                spec = spec.substr(p2 + 1);
                        }
                        else
                            spec = spec.substr(p1 + 1);
                    }
                    else if (!spec.empty() && spec[0] >= '0' && spec[0] <= '9')
                    {
                        fs = atoi(spec.c_str());
                        spec.clear();
                    }
                    // 颜色列表：连续 #RRGGBB（7 字符一组），其后为文本
                    size_t hp = spec.find('#');
                    if (hp != std::string::npos)
                    {
                        spec = spec.substr(hp); // 从第一个 # 起
                        while (spec.size() >= 7 && spec[0] == '#')
                        {
                            uint32_t c = 0;
                            bool ok = true;
                            for (int k = 1; k <= 6; k++)
                            {
                                char hc = spec[k];
                                int v = 0;
                                if (hc >= '0' && hc <= '9') v = hc - '0';
                                else if (hc >= 'a' && hc <= 'f') v = hc - 'a' + 10;
                                else if (hc >= 'A' && hc <= 'F') v = hc - 'A' + 10;
                                else
                                {
                                    ok = false;
                                    break;
                                }
                                c = (c << 4) | v;
                            }
                            if (!ok)
                                break;
                            colors.push_back(c);
                            spec = spec.substr(7);
                        }
                        text = spec; // 颜色列表后的剩余 = 文本
                    }
                    else
                    {
                        text = spec;
                    }
                    if (colors.empty())
                        colors.push_back(0xffffff);
                    // $字符串变量引用
                    if (!text.empty() && text[0] == '$')
                    {
                        std::string sv;
                        GetStrVar(text.substr(1), sv);
                        text = sv;
                    }
                    fprintf(stderr, "[ONS] -- lsp %d :s/ text='%s' fs=%d colors=%zu [%06x...]\n",
                            n, text.c_str(), fs, colors.size(),
                            (unsigned)(colors[0] & 0xffffff));
                    m_screen->SetSpriteTextCells(n, text, fs, colors);
                    st.textSprite = true;
                    st.file = text;
                    st.fontSize = fs;
                    st.color = colors[0];
                }
                else
                {
                    // 图片透明模式按上游类型解析（前缀/transmode），非启发式：
                    // 无前缀 + transmode alpha + jpg = 图对（右半 alpha 掩码）。
                    std::string sf;
                    int mode = ResolveImageSpec(file, sf, OnsScreen::IMG_ALPHA);
                    if (!sf.empty())
                    {
                        m_screen->SetSpriteImage(n, sf, mode);
                        st.file = sf; // 已解析存储名（存档恢复直接可用）
                        st.mode = mode;
                    }
                }
                m_screen->SetSpritePos(n, x, y);
                if (alpha >= 0)
                    m_screen->SetSpriteOpacity(n, alpha);
                RecordSpriteState(n, st);
            }
            m_screen->SetSpriteVisible(n, cmd != "lsph");
            return true;
        }
        if (cmd == "amsp" || cmd == "msp")
        {
            // amspCommand（command.cpp:3888）：绝对移动 x,y[,trans]；
            // mspCommand（1337）：相对移动 x+=dx,y+=dy[,trans+=]（动画步进，
            // 如 CLANNAD 校门镜头 for+wait 1 循环每帧 msp 201,0,1,0）。
            int x = toInt(arg(1), 0);
            int y = toInt(arg(2), 0);
            int alpha = -1;
            if (!arg(3).empty())
                alpha = toInt(arg(3), -1);
            auto it = m_spriteState.find(n);
            int nx = x, ny = y;
            if (cmd == "msp" && it != m_spriteState.end())
            {
                nx = it->second.x + x; // 相对：当前 + 增量
                ny = it->second.y + y;
            }
            else if (cmd == "msp")
            {
                // 无记录（层未初始化位置）：从层当前位置取
                tTJSNI_BaseLayer* l = m_screen ? m_screen->GetSpriteLayer(n) : nullptr;
                if (l)
                {
                    nx = l->GetLeft() + x;
                    ny = l->GetTop() + y;
                }
            }
            m_screen->SetSpritePos(n, nx, ny);
            if (it != m_spriteState.end())
            {
                it->second.x = nx;
                it->second.y = ny;
                if (alpha >= 0)
                {
                    // msp：trans 增量；amsp：trans 绝对（上游语义）
                    int na = (cmd == "msp") ? (it->second.alpha >= 0 ? it->second.alpha : 255) + alpha
                                            : alpha;
                    if (na < 0) na = 0;
                    if (na > 255) na = 255;
                    it->second.alpha = na;
                    m_screen->SetSpriteOpacity(n, na);
                }
            }
            else if (alpha >= 0)
                m_screen->SetSpriteOpacity(n, alpha);
            return true;
        }
        return true;
    }
    if (cmd == "ld")
    {
        // 立绘：ld l|c|r,"file"[,effect...]；位置按槽位计算（x=W*(n+1)/4-w/2，y=H-h）
        if (!m_screen)
            return true;
        std::string pos = ToLower(arg(0));
        if (pos == "l" || pos == "c" || pos == "r")
        {
            int slot = pos == "l" ? 0 : (pos == "c" ? 1 : 2);
            std::string file = arg(1);
            if (!file.empty())
            {
                // 立绘同 lsp 类型解析（无前缀 jpg = 图对，如 CGSH24B
                // 1280x480 = 640 立绘 + 640 alpha）
                std::string tf;
                int mode = ResolveImageSpec(EvalStringExpr(GetRawArg(args, 1)), tf,
                                            OnsScreen::IMG_ALPHA);
                if (!tf.empty())
                    m_screen->SetSpriteImage(TACHI_BASE + slot, tf, mode);
                else
                    m_screen->ClearSprite(TACHI_BASE + slot);
                tTJSNI_BaseLayer* l = m_screen->GetSpriteLayer(TACHI_BASE + slot);
                int w = l ? (int)l->GetWidth() : 0;
                int h = l ? (int)l->GetHeight() : 0;
                m_screen->SetSpritePos(TACHI_BASE + slot,
                                       m_screenW * (slot + 1) / 4 - w / 2,
                                       m_screenH - h);
                m_screen->SetSpriteVisible(TACHI_BASE + slot, true);
            }
            else
            {
                m_screen->ClearSprite(TACHI_BASE + slot);
            }
            return true;
        }
        // 经典形式 ld n,x,y,file（兼容）
        auto toInt = [&](const std::string& s, int dflt) {
            int v = dflt;
            EvalExpression(s, v);
            return v;
        };
        int n = toInt(arg(0), 0);
        int x = toInt(arg(1), 0);
        int y = toInt(arg(2), 0);
        std::string file = arg(3);
        if (!file.empty())
        {
            std::string sf;
            int mode = ResolveImageSpec(file, sf, OnsScreen::IMG_ALPHA);
            m_screen->SetSpriteImage(n, sf, mode);
            m_screen->SetSpritePos(n, x, y);
        }
        else
        {
            m_screen->ClearSprite(n);
        }
        return true;
    }

    // ---- 按钮：btndef/btn/spbtn/btnwait/textbtnwait ----
    if (cmd == "btndef" || cmd == "bclear")
    {
        // btndef [":a;按钮图集"]：重建按钮表（上游 deleteButtonLink）+ 设置按钮
        // 图集源（上游 btndef_info：btn 命令从其中裁剪按钮图像，不直接绘制）。
        if (cmd == "btndef" && !args.empty() && m_screen)
        {
            // 参数带引号（SplitArgs 保留），如 ever17
            //   btndef "background\"+$1+".jpg"  （"..."+$var+"..." 拼接）
            // 整段（含引号）交 EvalStringExpr：剥引号 + 按 '+' 分段 + $var 展开
            // ——预剥首尾引号会破坏 `\"` 配对（曾把文件名解析成 "background\+$1+.jpg"）。
            std::string a0 = args.empty() ? "" : args[0];
            size_t semi = a0.find(';');
            std::string file = (semi != std::string::npos) ? Trim(a0.substr(semi + 1)) : Trim(a0);
            std::string uf = Unquote(file);
            if (!uf.empty() && ToLower(uf) != "clear")
                m_screen->SetBtnSource(OnsResolveStorageName(EvalStringExpr(file)));
        }
        // 链重建：隐藏上一轮 btn 按钮精灵（上游 btndef 尾部 deleteButtonLink）
        HideBtnSprites();
        m_buttons.clear();
        return true;
    }
    if (cmd == "btn")
    {
        auto toInt = [&](const std::string& s, int dflt) {
            int v = dflt;
            EvalExpression(s, v);
            return v;
        };
        OnsButton b;
        b.id = toInt(arg(0), 0);
        b.x = toInt(arg(1), 0);
        b.y = toInt(arg(2), 0);
        b.w = toInt(arg(3), 0);
        b.h = toInt(arg(4), 0);
        m_buttons.push_back(b);
        fprintf(stderr, "[ONS] -- btn id=%d rect=(%d,%d %dx%d)\n", b.id, b.x, b.y, b.w, b.h);
        // 按钮图像（上游 btnCommand：从 btndef_info 图集裁剪 src_rect）：
        // btn id,x,y,w,h[,src_x,src_y] -> 精灵层 BTN_BASE+id
        if (m_screen && !args.empty() && args.size() >= 5)
        {
            int sx = args.size() > 5 ? toInt(arg(5), 0) : 0;
            int sy = args.size() > 6 ? toInt(arg(6), 0) : 0;
            m_screen->DrawBtnImage(OnsScreen::BTN_BASE + b.id, sx, sy, b.w, b.h, b.x, b.y);
            // 记录按钮精灵层：btnwait 返回/btndef 重建时隐藏（上游 show_flag=0）
            int layer = OnsScreen::BTN_BASE + b.id;
            if (std::find(m_btnSpriteIds.begin(), m_btnSpriteIds.end(), layer) ==
                m_btnSpriteIds.end())
                m_btnSpriteIds.push_back(layer);
        }
        return true;
    }
    if (cmd == "spbtn")
    {
        // spbtn sprite_no,id：把精灵矩形注册为按钮
        auto toInt = [&](const std::string& s, int dflt) {
            int v = dflt;
            EvalExpression(s, v);
            return v;
        };
        int n = toInt(arg(0), -1);
        int id = toInt(arg(1), n);
        tTJSNI_BaseLayer* l = n >= 0 ? m_screen->GetSpriteLayer(n) : nullptr;
        if (l)
        {
            OnsButton b;
            b.id = id;
            b.spriteNo = n;
            b.x = l->GetLeft();
            b.y = l->GetTop();
            b.w = (int)l->GetWidth();
            b.h = (int)l->GetHeight();
            m_buttons.push_back(b);
        }
        return true;
    }
    if (cmd == "exbtn")
    {
        // exbtn n,id,"CxxPyy控制串,..."：按钮矩形取精灵 n 的位置/尺寸；
        // 控制串为悬停效果（上游 exbtnCommand：Cxx=隐藏 sprite、Pxx=显示、
        // sN,(file)=播放音效；mouseOverCheck 悬停切换时执行）
        auto toInt = [&](const std::string& s, int dflt) {
            int v = dflt;
            EvalExpression(s, v);
            return v;
        };
        int n = toInt(arg(0), -1);
        int id = toInt(arg(1), n);
        tTJSNI_BaseLayer* l = n >= 0 && m_screen ? m_screen->GetSpriteLayer(n) : nullptr;
        if (l)
        {
            OnsButton b;
            b.id = id;
            b.spriteNo = n;
            b.x = l->GetLeft();
            b.y = l->GetTop();
            b.w = (int)l->GetWidth();
            b.h = (int)l->GetHeight();
            b.ctl = args.size() > 2 ? arg(2) : "";
            m_buttons.push_back(b);
            fprintf(stderr, "[ONS] -- exbtn sprite=%d id=%d rect=(%d,%d %dx%d)\n", n, id, b.x, b.y, b.w, b.h);
        }
        else
        {
            fprintf(stderr, "[ONS] -- exbtn sprite=%d id=%d FAILED (no layer)\n", n, id);
        }
        return true;
    }
    if (cmd == "exbtn_d")
    {
        // exbtn_d "Pxx..Cxx.."：悬停离开时恢复所有按钮普通态（上游
        // exbtn_d_button_link：mouseOverCheck 每次悬停变化时执行）
        m_exbtnD = args.empty() ? "" : Unquote(args[0]);
        return true;
    }
    if (cmd == "btnwait" || cmd == "btnwait2" || cmd == "textbtnwait" ||
        cmd == "selectbtnwait")
    {
        // btnwait %var：等待点击；结果：按钮 id / 0=普通左键 / -1=右键 / 2=中键 / -3=ESC
        // 上游 btnwaitCommand：btnwait（del_flag）/btnwait2 先 leaveTextDisplayMode，
        // textbtnwait 不离开（等待期间文本窗保持）。selectbtnwait = btnwaitCommand
        // 同实现（csel 选择等待；按钮由脚本 csp 自清，delete=false）。
        if (cmd == "btnwait" || cmd == "btnwait2")
            LeaveTextDisplayMode(false);
        std::string v = arg(0);
        if (!v.empty() && v[0] == '%')
            v = v.substr(1);
        if (v.empty())
            v = "0";
        m_btnwaitVar = v;
        m_btnwaitDelete = (cmd == "btnwait");
        fprintf(stderr, "[ONS] -- %s -> var=%s\n", cmd.c_str(), m_btnwaitVar.c_str());
        m_waitStart = OnsGetTicks();
        m_wait = WM_BTNWAIT;
        return false;
    }

    // ---- 文本显示模式命令（上游 command.cpp texton/textoff）----
    if (cmd == "textoff")
    {
        // textoff：leaveTextDisplayMode(true)（强制离开，无视 erasetextwindow）
        LeaveTextDisplayMode(true);
        return true;
    }
    if (cmd == "texton")
    {
        // texton：enterTextDisplayMode（窗背由下一页 DrawTextPage 重绘）
        EnterTextDisplayMode();
        return true;
    }
    if (cmd == "texthide")
    {
        // texthide：画面刷新时临时隐藏文本（上游 REFRESH_SHADOW_MODE flush）。
        // Layer 模型下等价于清文本层；下一段文本/texthide 解除后重绘。
        if (m_screen)
            ClearText();
        return true;
    }

    // ---- 选择支：select（goto 模式）/ selnum（写变量模式）----
    if (cmd == "getparam" || cmd == "getparam2")
    {
        // getparam %a,$b,...：从调用行读取子例程参数（上游 ScriptParser）
        if (m_subParams.empty())
            return true;
        auto& params = m_subParams.back();
        size_t pi = 0;
        for (size_t i = 0; i < args.size() && pi < params.size(); i++)
        {
            std::string v = arg(i);
            if (v.empty())
                continue;
            if (v[0] == '$')
            {
                SetStrVar(v.substr(1), params[pi]);
            }
            else
            {
                int val = 0;
                std::string pv = params[pi];
                if (pv.size() >= 2 && (pv[0] == '"' || pv[0] == '\''))
                    pv = pv.substr(1, pv.size() - 2);
                EvalExpression(pv, val);
                if (v[0] == '%')
                    v = v.substr(1);
                SetVar(v, val);
            }
            pi++;
        }
        return true;
    }
    if (cmd == "len")
    {
        // len %var,"字符串"：字节长度 -> %var（上游 ScriptParser lenCommand）
        std::string v = arg(0);
        if (!v.empty() && v[0] == '%')
            v = v.substr(1);
        int n = 0;
        if (args.size() > 1)
        {
            // 字节数按脚本原编码口径（GBK/日文：CJK 2 字节、ASCII 1 字节）。
            // 内部已转 UTF-8（CJK 3 字节），换算回 2 字节/汉字 —— 否则
            // ever17 *customsel 的 len*7 像素估算偏大 1.5 倍，选项按钮错位。
            std::string sv = SubstituteVars(Unquote(args[1]));
            for (size_t i = 0; i < sv.size();)
            {
                unsigned char c = (unsigned char)sv[i];
                if (c < 0x80)
                {
                    n += 1;
                    i += 1;
                }
                else if (c < 0xE0)
                {
                    n += 1; // 2 字节 UTF-8（扩展拉丁等）在 GBK 多为 1 字节
                    i += 2;
                }
                else
                {
                    n += 2; // 3 字节 UTF-8 = CJK（GBK 2 字节）
                    i += (c < 0xF0) ? 3 : 4;
                }
            }
        }
        SetVar(v, n);
        return true;
    }
    if (cmd == "mov")
    {
        // mov "file"[,click][,loop]（上游 movieCommand）：播放视频，播完/点击后继续
        std::string file = EvalStringExpr(GetRawArg(args, 0));
        if (!file.empty() && m_movie)
        {
            bool click = false, loop = false;
            for (size_t i = 1; i < args.size(); i++)
            {
                std::string a = ToLower(arg(i));
                if (a == "click")
                    click = true;
                else if (a == "loop")
                    loop = true;
            }
            m_movie->Play(OnsResolveStorageName(file), click, loop);
            m_movieClick = click;
            m_wait = WM_MOVIE;
            return false;
        }
        return true;
    }
    if (cmd == "mpegplay")
    {
        // mpegplay "file",click(0/1)（上游 mpegplayCommand）
        std::string file = EvalStringExpr(GetRawArg(args, 0));
        if (!file.empty() && m_movie)
        {
            bool click = false;
            int c = 0;
            if (!arg(1).empty() && EvalExpression(arg(1), c))
                click = c != 0;
            m_movie->Play(OnsResolveStorageName(file), click, false);
            m_movieClick = click;
            m_wait = WM_MOVIE;
            return false;
        }
        return true;
    }
    if (cmd == "csel")
    {
        // 上游 selectCommand 的 csel 模式：只解析选项对存入选择链，
        // 然后 setCurrentLabel("customsel") —— 选项 UI 由脚本 *customsel
        // 例程自绘（getcselstr 取文本 + cselbtn 注册按钮 + selectbtnwait
        // 等待 + cselgoto 跳结果）。选项数据保留在 m_selectItems 供
        // getcselnum/getcselstr/cselbtn 读取。
        std::vector<std::string> toks;
        if (!SplitQuotedTokens(args.empty() ? "" : args[0], toks))
            return true;
        m_selectItems.clear();
        m_selectMode = 0;
        for (size_t i = 0; i + 1 < toks.size(); i += 2)
        {
            SelectItem it;
            it.text = toks[i];
            if (!toks[i + 1].empty() && toks[i + 1][0] == '*')
                it.label = toks[i + 1].substr(1);
            m_selectItems.push_back(it);
        }
        auto it = m_labels.find("customsel");
        if (it != m_labels.end())
        {
            m_pc = it->second; // 跳 *customsel（标签行为空，Step 自 +1 执行例程）
            return true;
        }
        // 脚本无 *customsel：回退内置选择菜单（同 select）
        if (m_selectItems.empty())
            return true;
        m_selectMenuY = m_textCursorY;
        DrawSelectMenu();
        m_wait = WM_SELECT;
        return false;
    }
    if (cmd == "select" || cmd == "selnum")
    {
        // csel 与 select 同款（上游 selectCommand：csel = SELECT_CSEL_MODE，
        // 仅多 saveon 自动存档，本项目不自动存档）
        std::vector<std::string> toks;
        if (!SplitQuotedTokens(args.empty() ? "" : args[0], toks))
            return true;
        m_selectItems.clear();
        m_selectCursor = 0;
        if (cmd == "selnum")
        {
            // selnum %var%,"项1","项2",...
            m_selectMode = 1;
            if (toks.empty())
                return true;
            m_selectVar = toks[0];
            for (size_t i = 1; i < toks.size(); i++)
            {
                SelectItem it;
                it.text = toks[i];
                m_selectItems.push_back(it);
            }
        }
        else
        {
            // select "项1",*标签1,"项2",*标签2,...
            m_selectMode = 0;
            for (size_t i = 0; i + 1 < toks.size(); i += 2)
            {
                SelectItem it;
                it.text = toks[i];
                if (!toks[i + 1].empty() && toks[i + 1][0] == '*')
                    it.label = toks[i + 1].substr(1);
                m_selectItems.push_back(it);
            }
            if (toks.size() % 2 == 1)
            {
                SelectItem it;
                it.text = toks.back();
                m_selectItems.push_back(it);
            }
        }
        if (m_selectItems.empty())
            return true;
        m_selectMenuY = m_textCursorY;
        DrawSelectMenu();
        m_wait = WM_SELECT;
        return false;
    }
    if (cmd == "getcselnum")
    {
        // getcselnum %v：csel 选项个数（上游 getcselnumCommand 数 SelectLink）
        std::string v = arg(0);
        if (!v.empty() && v[0] == '%')
            v = v.substr(1);
        SetVar(v, (int)m_selectItems.size());
        return true;
    }
    if (cmd == "getcselstr")
    {
        // getcselstr $dst,idx：第 idx 个 csel 选项文本
        std::string v = arg(0);
        if (!v.empty() && v[0] == '$')
            v = v.substr(1);
        int idx = 0;
        if (!arg(1).empty())
            EvalExpression(arg(1), idx);
        SetStrVar(v, (idx >= 0 && idx < (int)m_selectItems.size()) ? m_selectItems[idx].text
                                                                   : "");
        return true;
    }
    if (cmd == "cselbtn")
    {
        // cselbtn csel_no,btn_no,x,y：把第 csel_no 个选项文本做成按钮
        // （上游 cselbtnCommand：getSelectableSentence 文本按钮插按钮链，
        // 文字画在 (x,y)，btnwait/selectbtnwait 返回 btn_no）。按钮精灵槽 =
        // BTN_BASE+btn_no（btn 同区）；命中区 = 文字宽高外扩。
        int cselNo = 0, btnNo = 0, x = 0, y = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), cselNo);
        if (!arg(1).empty())
            EvalExpression(arg(1), btnNo);
        if (!arg(2).empty())
            EvalExpression(arg(2), x);
        if (!arg(3).empty())
            EvalExpression(arg(3), y);
        if (cselNo < 0 || cselNo >= (int)m_selectItems.size() || !m_screen)
            return true;
        const std::string& text = m_selectItems[cselNo].text;
        if (text.empty())
            return true;
        std::vector<uint32_t> colors;
        colors.push_back(0xffffff);
        int slot = OnsScreen::BTN_BASE + btnNo;
        m_screen->SetSpriteTextCells(slot, text, m_fontSize, colors);
        m_screen->SetSpritePos(slot, x, y);
        m_screen->SetSpriteVisible(slot, true);
        m_screen->MarkSpriteAsBtn(slot); // 恒最上层组（选项浮于立绘/文本之上）
        tTJSNI_BaseLayer* l = m_screen->GetSpriteLayer(slot);
        int w = l ? (int)l->GetWidth() : 200;
        int h = l ? (int)l->GetHeight() : m_lineSpacing;
        OnsButton b;
        b.id = btnNo;
        b.x = x;
        b.y = y;
        b.w = w + 20; // 外扩：点击框内空白也命中
        b.h = std::max(h + 8, m_lineSpacing);
        m_buttons.push_back(b);
        fprintf(stderr, "[ONS] -- cselbtn opt=%d btn=%d text='%s' at (%d,%d %dx%d)\n",
                cselNo, btnNo, text.c_str(), b.x, b.y, b.w, b.h);
        return true;
    }
    if (cmd == "cselgoto")
    {
        // cselgoto idx：跳第 idx 个选项的标签（上游 cselgotoCommand：
        // setCurrentLabel + deleteSelectLink + newPage）。
        // 参数可能带空格（"cselgoto %0 - 500"）被 SplitArgs 拆开 —— 重拼后求值
        int idx = 0;
        if (!args.empty())
        {
            std::string expr;
            for (auto& a : args)
                expr += a;
            EvalExpression(expr, idx);
        }
        if (idx >= 0 && idx < (int)m_selectItems.size())
        {
            const std::string& lbl = m_selectItems[idx].label;
            if (!lbl.empty())
            {
                auto it = m_labels.find(ToLower(lbl));
                if (it != m_labels.end())
                    m_pc = it->second;
            }
        }
        ClearText();
        return true;
    }
    if (cmd == "skipoff")
    {
        // skipoffCommand：skip_mode &= ~SKIP_NORMAL
        m_skipMode = false;
        return true;
    }

    // ---- 音频（M3）：bgm/bgmstop/playstop/se/playvoice/dwave/dwavestop ----
    if (cmd == "bgm")
    {
        std::string file = arg(0);
        if (!file.empty() && file[0] != '"' && !arg(1).empty())
            file = arg(1); // bgm n,"file" 形式（n=循环点，忽略）
        if (m_audio)
            m_audio->PlayBgm(OnsResolveStorageName(EvalStringExpr(GetRawArg(args, 0))));
        return true;
    }
    if (cmd == "bgmstop" || cmd == "playstop")
    {
        if (m_audio)
        {
            if (cmd == "bgmstop")
                m_audio->StopBgm();
            else
                m_audio->StopAll();
        }
        return true;
    }
    if (cmd == "se")
    {
        if (m_audio)
            m_audio->PlaySe(OnsResolveStorageName(EvalStringExpr(GetRawArg(args, 0))));
        return true;
    }
    if (cmd == "playvoice" || cmd == "dwave" || cmd == "dwaveloop" || cmd == "dwaveload" ||
        cmd == "dwaveplay" || cmd == "dwaveplayloop")
    {
        // 上游 dwaveCommand（command.cpp:2903）：readInt 读通道 -> readStr 读文件；
        // 相邻形式 `dwave 0"w\s01.ogg"`（通道后直接跟引号）是合法脚本。
        // dwaveload=预载不播；dwaveloop/dwaveplayloop=循环（本项目暂按一次性，
        // 循环语音待 M5）；通道夹取 [0, VOICE_CHANNELS)。
        std::string a0 = arg(0);
        int ch = 0;
        std::string file;
        size_t p = 0;
        int v = 0;
        while (p < a0.size() && a0[p] >= '0' && a0[p] <= '9')
        {
            v = v * 10 + (a0[p] - '0');
            p++;
        }
        if (p > 0)
        {
            ch = v;
            // 数字后直接跟引号：剩余即文件名（相邻形式）
            if (p < a0.size() && (a0[p] == '"' || a0[p] == '\''))
                file = Trim(a0.substr(p)); // 保留引号：EvalStringExpr 按整串解析
            else
                file = GetRawArg(args, 1);
        }
        else
        {
            if (EvalExpression(a0, v))
                ch = v;
            file = GetRawArg(args, 1);
        }
        // 文件参数必须带完整引号喂 EvalStringExpr（剥引号 + ".."+$var+.. 拼接）：
        // Unquote 后再展开会把 `\"` 的引号当成开引号，$var 落入引号内不展开
        // （曾致 voice\"+$0+".ogg 原样 resolve 失败、语音不播）
        std::string resolved = OnsResolveStorageName(EvalStringExpr(file));
        fprintf(stderr, "[ONS] -- %s ch=%d file='%s' -> '%s'\n", cmd.c_str(), ch,
                file.c_str(), resolved.c_str());
        if (cmd == "dwaveload")
            return true; // 预载语义暂未实现（M5）
        if (m_audio && !resolved.empty())
            m_audio->PlayVoice(ch, resolved);
        return true;
    }
    if (cmd == "dwavestop")
    {
        int ch = 0;
        if (!arg(0).empty())
        {
            int v = 0;
            if (EvalExpression(arg(0), v))
                ch = v;
        }
        if (m_audio)
            m_audio->StopVoice(ch);
        return true;
    }
    // ---- 音量（上游 music_volume/se_volume/voice_volume，0-100）----
    if (cmd == "bgmvol")
    {
        // mp3volCommand：music_volume = n; Mix_VolumeMusic(n*MAX/100)
        int v = 100;
        if (!arg(0).empty())
            EvalExpression(arg(0), v);
        if (m_audio)
            m_audio->SetBgmVolume(v);
        return true;
    }
    if (cmd == "sevol")
    {
        // sevolCommand：se_volume = n；应用到全部 SE 通道
        int v = 100;
        if (!arg(0).empty())
            EvalExpression(arg(0), v);
        if (m_audio)
            m_audio->SetSeVolume(v);
        return true;
    }
    if (cmd == "voicevol")
    {
        // voicevolCommand：voice_volume = n；应用到语音通道
        int v = 100;
        if (!arg(0).empty())
            EvalExpression(arg(0), v);
        if (m_audio)
            m_audio->SetVoiceVolume(v);
        return true;
    }
    if (cmd == "defmp3vol" || cmd == "defsevol" || cmd == "defvoicevol" ||
        cmd == "mp3vol")
    {
        // defmp3volCommand（上游）：默认 BGM 音量（registry 持久）；
        // 简化：立即应用到当前 BGM 并记录（重启后由 gloval 变量恢复后续做）。
        int v = 100;
        if (!arg(0).empty())
            EvalExpression(arg(0), v);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (cmd == "defsevol")
        {
            m_sevol = v;
            if (m_audio) m_audio->SetSeVolume(v);
        }
        else if (cmd == "defvoicevol")
        {
            m_voicevol = v;
            if (m_audio) m_audio->SetVoiceVolume(v);
        }
        else
        {
            m_mp3vol = v;
            if (m_audio) m_audio->SetBgmVolume(v);
        }
        fprintf(stderr, "[ONS] -- %s -> %d\n", cmd.c_str(), v);
        return true;
    }
    if (cmd == "getbgmvol" || cmd == "getsevol" || cmd == "getvoicevol")
    {
        // getmp3volCommand/getsevolCommand/getvoicevolCommand：当前音量 -> 变量
        std::string v = arg(0);
        if (!v.empty() && v[0] == '%')
            v = v.substr(1);
        if (!v.empty() && m_audio)
        {
            int cur = cmd == "getbgmvol" ? m_audio->GetBgmVolume()
                      : (cmd == "getsevol" ? m_audio->GetSeVolume()
                                           : m_audio->GetVoiceVolume());
            SetVar(v, cur);
        }
        return true;
    }

    // ---- 系统 ----
    if (cmd == "systemcall")
    {
        // 上游 systemcallCommand：system_menu_mode = getSystemCallNo(label)，
        // 在 waitEvent 内 executeSystemCall 执行；本项目同步执行等价动作。
        std::string name = ToLower(arg(0));
        fprintf(stderr, "[ONS] -- systemcall %s\n", name.c_str());
        if (name == "end")
        {
            m_finished = true;
            return false;
        }
        if (name == "automode")
        {
            // SYSTEM_AUTOMODE（rm:246）：automode_flag=true；文本等待定时自动
            // 推进；任意真实点击/按键先关（点一下停，不推进）
            m_autoMode = true;
            m_skipMode = false;
            m_waitStart = OnsGetTicks();
            return true;
        }
        if (name == "skip")
        {
            // SYSTEM_SKIP（rm:240）：skip_mode |= SKIP_NORMAL
            m_skipMode = true;
            m_autoMode = false;
            m_waitStart = OnsGetTicks();
            return true;
        }
        if (name == "windowerase")
        {
            // SYSTEM_WINDOWERASE（rm:276 executeWindowErase）：隐藏文本框
            // （display_mode NORMAL），等一次点击恢复
            if (m_screen)
            {
                tTJSNI_BaseLayer* wl = m_screen->GetWindowLayer();
                if (wl)
                {
                    wl->SetVisible(false);
                    wl->Update();
                }
                tTJSNI_BaseLayer* tl = m_screen->GetTextLayer();
                if (tl)
                {
                    tl->SetVisible(false);
                    tl->Update();
                }
            }
            m_erasePending = true;
            m_erasePrevWait = 0; // 脚本内 systemcall：恢复后脚本继续
            m_wait = WM_ERASE;
            m_waitStart = OnsGetTicks();
            return false;
        }
        if (name == "lookback")
        {
            // SYSTEM_LOOKBACK（rm:625 executeSystemLookback）：回看文本历史
            if (m_textHistory.empty())
                return true; // 无历史：直接返回（脚本继续）
            m_lookbackIdx = (int)m_textHistory.size();
            LookbackPage(-1); // 定位到最近一页（显示末尾 rows 句）
            m_wait = WM_LOOKBACK;
            return false;
        }
        if (name == "save")
        {
            SaveState(0);
            return true;
        }
        if (name == "load")
        {
            LoadState(0);
            return true;
        }
        return true; // reset/rmenu 等：无（CLANNAD 由脚本 goto 处理）
    }
    if (cmd == "savegame")
    {
        int slot = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), slot);
        fprintf(stderr, "[ONS] -- savegame slot=%d\n", slot);
        SaveState(slot);
        return true;
    }
    if (cmd == "loadgame")
    {
        int slot = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), slot);
        fprintf(stderr, "[ONS] -- loadgame slot=%d\n", slot);
        bool ok = LoadState(slot);
        fprintf(stderr, "[ONS] -- loadgame result=%d pc=%d\n", ok ? 1 : 0, m_pc);
        return true;
    }
    if (cmd == "reset")
    {
        // 回到 *start（NScr resetCommand：只复位 local 变量；globalon 时
        // global_variable_border=0，全部变量保留 —— upstream resetCommand:1125）
        if (!m_globalon)
            m_vars.clear();
        m_callStack.clear();
        m_loops.clear();
        ResetToStart();
        auto it = m_labels.find("start");
        if (it != m_labels.end())
            m_pc = it->second - 1;
        return true;
    }
    if (cmd == "split" || cmd == "splitstring")
    {
        // split $src,"delim",$out1[,$out2...]（上游 splitCommand）：
        // 原串（$变量/字面量，EvalStringExpr 展开）按分隔符逐段切出写入
        // 输出变量（$str 直接存，%int/? 数组 atoi）。脚本已转 UTF-8，
        // 按 UTF-8 字符边界推进（对齐 force_utf8 的 UTF8_N_BYTE 跳）。
        std::string src = EvalStringExpr(args.empty() ? "" : args[0]);
        auto utf8chars = [](const std::string& s) {
            std::vector<std::string> out;
            for (size_t i = 0; i < s.size();)
            {
                unsigned char c = (unsigned char)s[i];
                size_t n = 1;
                if (c >= 0xF0)
                    n = 4;
                else if (c >= 0xE0)
                    n = 3;
                else if (c >= 0xC0)
                    n = 2;
                out.push_back(s.substr(i, n));
                i += n;
            }
            return out;
        };
        // 分隔符取首个字符（上游 delimiter = readStr()[0]）
        std::string d;
        if (args.size() >= 2)
        {
            std::string ds = EvalStringExpr(args[1]);
            auto dc = utf8chars(ds);
            if (!dc.empty())
                d = dc[0];
        }
        if (d.empty())
            d = ",";
        std::vector<std::string> chars = utf8chars(src);
        size_t pos = 0; // 字符下标
        for (size_t ai = 2; ai < args.size(); ai++)
        {
            std::string tok;
            while (pos < chars.size() && chars[pos] != d)
                tok += chars[pos++];
            std::string v = Trim(args[ai]);
            if (!v.empty() && v[0] == '"')
            {
                // 字面量输出参数（无意义）忽略
            }
            else if (!v.empty() && v[0] == '$')
            {
                SetStrVar(v.substr(1), tok);
            }
            else if (!v.empty() && (v[0] == '%' || v[0] == '?'))
            {
                SetVar(v, atoi(tok.c_str()));
            }
            if (pos < chars.size())
                pos++; // 跳过分隔符
        }
        return true;
    }
    if (cmd == "itoa" || cmd == "itoa2")
    {
        // itoa/itoa2 $dst,src：整数转字符串写入 $变量（itoa2 上游与 itoa 相同，%d）
        std::string v = arg(0);
        if (!v.empty() && v[0] == '$')
            v = v.substr(1);
        int val = 0;
        if (!arg(1).empty())
            EvalExpression(arg(1), val);
        char buf[24];
        snprintf(buf, sizeof(buf), "%d", val);
        SetStrVar(v, buf);
        return true;
    }
    if (cmd == "savefileexist")
    {
        // savefileexist %dst,slot：检查存档槽位是否存在
        std::string v = arg(0);
        if (!v.empty() && v[0] == '%')
            v = v.substr(1);
        int slot = 0;
        if (!arg(1).empty())
            EvalExpression(arg(1), slot);
        std::ifstream f(SaveSlotName(m_saveDir, slot));
        SetVar(v, f.good() ? 1 : 0);
        return true;
    }
    if (cmd == "savetime")
    {
        // savetime slot,%mo,%day,%ho,%mi：读存档时间（月/日/时/分）
        int slot = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), slot);
        auto setVar = [&](size_t i, int val) {
            std::string v = arg(i);
            if (!v.empty() && v[0] == '%')
                v = v.substr(1);
            if (!v.empty())
                SetVar(v, val);
        };
        time_t t = 0;
        {
            std::ifstream f(SaveSlotName(m_saveDir, slot));
            std::string key;
            long long tval = 0;
            while (f >> key)
            {
                if (key == "time")
                {
                    f >> tval;
                    break;
                }
            }
            t = (time_t)tval;
        }
        if (!t)
        {
            setVar(1, 0);
            setVar(2, 0);
            setVar(3, 0);
            setVar(4, 0);
            return true;
        }
        struct tm tmv;
#if _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        setVar(1, tmv.tm_mon + 1);
        setVar(2, tmv.tm_mday);
        setVar(3, tmv.tm_hour);
        setVar(4, tmv.tm_min);
        return true;
    }
    if (cmd == "rnd")
    {
        // rnd %var,n
        std::string v = arg(0);
        if (!v.empty() && v[0] == '%')
            v = v.substr(1);
        int n = 0;
        EvalExpression(arg(1), n);
        if (n > 0)
            SetVar(v, rand() % n);
        return true;
    }
    if (cmd == "erasetextwindow")
    {
        // 上游（Onscripter erasetextwindowCommand）：只设 erase_text_window_mode，
        // 控制离开文本显示模式时是否擦除窗口（0=保留，!=0 擦除）。不改变层可见性。
        int n = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), n);
        m_eraseTextWindow = n;
        return true;
    }
    if (cmd == "caption")
    {
        TVPSetWindowTitle(arg(0).c_str());
        return true;
    }
    if (cmd == "effect" && !args.empty())
    {
        // DEFINE 段注册特效：effect no,effect[,duration[,image]]
        // （上游 effectCommand + readEffect：no 为注册键，effect 为特效编号）。
        // CLANNAD：effect 2,10,500 -> 键 2 = crossfade(10) 500ms。M3.2 接入转场系统。
        int no = 0, e = 0, dur = 0;
        EvalExpression(arg(0), no);
        if (args.size() > 1)
            EvalExpression(arg(1), e);
        if (args.size() > 2)
            EvalExpression(arg(2), dur);
        if (no >= 2 && no <= 255)
            m_effects[no] = std::make_pair(e, dur);
        return true;
    }
    if (cmd == "effectblank")
    {
        // 默认特效号（bg 等未显式带特效参数时使用；上游 effectblankCommand）
        EvalExpression(arg(0), m_effectBlank);
        return true;
    }
    if (cmd == "transmode")
    {
        // 默认透明模式（上游 transmodeCommand：alpha/rightcopy/leftcopy/copy，
        // 影响其后 lsp 等精灵的透明处理）。暂只记录（demo 用 alpha 默认）。
        m_transMode = arg(0);
        return true;
    }
    if (cmd == "windoweffect")
    {
        // 文本窗进出转场（上游 effectCommand 的 windoweffect 分支；
        // readEffect 格式：effect[,duration[,image]]）
        int e = 0, dur = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), e);
        if (!arg(1).empty())
            EvalExpression(arg(1), dur);
        m_windowEffect = e;
        m_windowEffectDur = dur;
        return true;
    }
    if (cmd == "windowback")
    {
        // windowbackCommand：windowback_flag=true（DEFINE 段声明文本框进出使用
        // window_effect；默认 window_effect=1/即时，CLANNAD 未设 windoweffect）。
        m_windowbackFlag = true;
        return true;
    }
    if (cmd == "saveon")
    {
        // saveonCommand：saveon_flag = true（上游 autosaveoff 时忽略）
        m_saveon = true;
        return true;
    }
    if (cmd == "saveoff")
    {
        // saveoffCommand：saveon_flag = false（上游还会触发一次 storeSaveFile；
        // 本项目存档仅显式触发，不随页自动写档，见 docs/07 §5）
        m_saveon = false;
        return true;
    }
    if (cmd == "print")
    {
        // printCommand（command.cpp:1246）：leaveTextDisplayMode + parseEffect(true) +
        // doEffect。CLANNAD 标题 print 10,500 = 文本窗 crossfade 淡出。
        // 特效时长 M3.2 接入；现阶段直接离开文本模式（等价 effect 0/1 即时）。
        LeaveTextDisplayMode(false);
        return true;
    }
    if (cmd == "texec")
    {
        // texecCommand（command.cpp:295）：CLICK_NEWPAGE -> newPage()（清文本）；
        // CLICK_WAIT|CLICK_EOL -> processEOT()；最后 saveon_flag=true。
        // 页断清文本点：textgosub 例程（CLANNAD *end_textbtn）在此收尾一页
        // （点击后旧文字清除、窗背由下个 DrawTextPage 的页首重载）。
        ClearText();
        m_pageBreakPending = false;
        m_saveon = true;
        return true;
    }
    if (cmd == "repaint")
    {
        // repaintCommand：立即重绘。krkrsdl3 Layer 模型每次 Update 已标脏，
        // 下一帧自动合成 —— 等价实现，无需额外操作。
        return true;
    }
    if (cmd == "br")
    {
        // brCommand：换页 newPage()（清文本缓冲，窗背保持；上游 ONScripter.cpp:1320）
        ClearText();
        return true;
    }
    if (cmd == "quake" || cmd == "quakey" || cmd == "quakex")
    {
        // quakeCommand（command.cpp:1142）：quake <no>,<duration>；
        // quakey=上下正弦 quakex=左右正弦 quake=随机抖动（no=强度 1..3）；
        // duration < no*4 时取 no*4；阻塞执行（上游 while(doEffect)）。
        int type = cmd == "quakey" ? 0 : (cmd == "quakex" ? 1 : 2);
        int no = 0, dur = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), no);
        if (!arg(1).empty())
            EvalExpression(arg(1), dur);
        if (no <= 0)
            return true;
        if (dur < no * 4)
            dur = no * 4;
        if (m_screen && m_screen->StartQuake(type, no, dur))
        {
            m_wait = WM_EFFECT;
            m_waitUntil = OnsGetTicks() + (uint32_t)dur;
            return false;
        }
        return true;
    }
    if (cmd == "getscreenshot")
    {
        // getscreenshot w,h：设定截图目标尺寸并立即快照当前画面（上游
        // getscreenshotCommand：accumulation_surface -> screenshot_surface）
        int w = 0, h = 0;
        if (!arg(0).empty())
            EvalExpression(arg(0), w);
        if (!arg(1).empty())
            EvalExpression(arg(1), h);
        if (w <= 0)
            w = 160;
        if (h <= 0)
            h = 120;
        m_shotW = w;
        m_shotH = h;
        if (m_screen)
        {
            int fw = 0, fh = 0;
            std::vector<uint32_t> buf;
            if (m_screen->CompositeSnapshot(buf, fw, fh))
            {
                m_shotBuffer.swap(buf);
                m_shotFullW = fw;
                m_shotFullH = fh;
            }
        }
        return true;
    }
    if (cmd == "savescreenshot" || cmd == "savescreenshot2")
    {
        // savescreenshot file：快照缩放到 getscreenshot 尺寸，写到 save_dir/文件名
        // （上游 savescreenshotCommand：resizeSurface + SDL_SaveBMP_RW；目录部分剥离；
        // 用 getscreenshot 时的内存快照，无快照则现拍）
        std::string file = EvalStringExpr(GetRawArg(args, 0));
        if (file.empty())
            return true;
        std::string base = file;
        size_t sp = base.find_last_of("/\\");
        if (sp != std::string::npos)
            base = base.substr(sp + 1);
        TakeScreenshot(m_shotW, m_shotH, base);
        return true;
    }
    if (cmd == "deletescreenshot")
    {
        // deletescreenshotCommand：释放内存快照
        m_shotBuffer.clear();
        return true;
    }
    if (cmd == "humanz")
    {
        // humanzCommand（ScriptParser_command.cpp:1031）：z_order = N ——
        // 精灵 z 分界（>z 背景层在窗背文字下；<=z 前景层在上）。CLANNAD
        // DEFINE 段 humanz 199。执行期出现同样生效（重排）。
        int z = 499;
        if (!arg(0).empty())
            EvalExpression(arg(0), z);
        m_zOrder = z;
        if (m_screen)
            m_screen->SetZOrder(z);
        return true;
    }
    // DEFINE 段设置命令 / 系统配置命令：执行期出现时忽略（继续执行）。
    // （rmenu/menusetwindow 等真实逻辑在 LoadDefineSection 解析）
    if (cmd == "kidokuskip" || cmd == "globalon" || cmd == "nsa" ||
        cmd == "savename" || cmd == "savenumber" || cmd == "versionstr" || cmd == "lookbacksp" ||
        cmd == "lookbackcolor" || cmd == "menusetwindow" || cmd == "menuselectcolor" ||
        cmd == "selectcolor" || cmd == "rmenu" || cmd == "load" || cmd == "game" ||
        cmd == "bar" || cmd == "prnum" || cmd == "automode" || cmd == "resettimer" ||
        cmd == "getenter" || cmd == "useescspc")
    {
        return true;
    }

    // 已知但未实现的命令：占位 + 诊断日志
    fprintf(stderr, "[ONS] -- UNIMPLEMENTED cmd='%s' args=%zu\n", cmd.c_str(), args.size());
    for (size_t i = 0; i < args.size() && i < 6; i++)
        fprintf(stderr, "[ONS] --   arg[%zu]='%s'\n", i, args[i].c_str());
    return true;
}

void OnsVM::DrawTextLine(const std::string& text, bool appendToCurrent)
{
    if (!m_screen || !m_screen->GetTextLayer())
        return;
    if (text.empty())
    {
        if (!appendToCurrent)
            m_textCursorY += m_lineSpacing;
        return;
    }
    tTJSNI_BaseLayer* tl = m_screen->GetTextLayer();
    // 自动换行（上游 checkLineBreak/isEndOfLine 的简化）：按文本层宽度断行，
    // 超出宽度的长行分段绘制，避免文字画出文本框
    int maxChars = std::max(1, (m_textW > 0 ? m_textW : 600) / std::max(1, m_fontSize));
    size_t pos = 0;
    while (pos < text.size())
    {
        int chars = 0;
        size_t end = pos;
        while (end < text.size() && chars < maxChars)
        {
            unsigned char c = (unsigned char)text[end];
            end += (c < 0x80) ? 1 : ((c < 0xE0) ? 2 : ((c < 0xF0) ? 3 : 4));
            chars++;
        }
        std::string seg = text.substr(pos, end - pos);
        // 接续模式（裸 @ 后段）：画在当前行尾继续；否则正常换行（页满先清）
        if (appendToCurrent)
        {
            if (m_textLineAdvance + chars * m_fontSize > m_textW)
            {
                // 接续段超出窗口宽：换行
                m_textCursorY += m_lineSpacing;
                m_textLineAdvance = 0;
            }
        }
        else
        {
            // 页满换页：本段放不下时清掉旧文本（上游页满 newPage 语义；窗背保持）。
            // 页满按"窗背内文字起点(baseY)起的已用行数"判断 —— 窗背通常比文本区
            // 大（ever17 messwin 800x368 贴 (0,230)，首行文字层内 y=435-230=205，
            // 用绝对 cursorY 比对 m_textH 会把每行都误判为页满 -> ClearText 清掉
            // 窗背并把文字打到层内 (0,0) = 屏幕中部："文字没进对话框"的根因。
            int baseY = m_textY - m_winY;
            if (m_textCursorY - baseY + m_lineSpacing > m_textH)
            {
                ClearText();
                m_textPageFresh = false; // 下面立即重载窗背（同页剩余段续画）
                if (!m_windowImage.empty() && !m_windowColorMode)
                    m_screen->SetTextLayerWindowImage(OnsResolveStorageName(m_windowImage),
                                                      m_winX, m_winY);
                // 新页：文字回到窗背内首行起点（ClearText 不再重置光标）
                m_textCursorY = baseY;
                m_textCursorX = m_textX - m_winX;
            }
            m_textLineAdvance = 0;
        }
        fprintf(stderr, "[ONS] -- textline layer=(%d,%d %dx%d) cur=(%d,%d)\n",
                (int)tl->GetLeft(), (int)tl->GetTop(), (int)tl->GetWidth(),
                (int)tl->GetHeight(), m_textCursorX, m_textCursorY);
        tl->DrawText(m_textCursorX + m_textLineAdvance, m_textCursorY,
                     ttstr(seg.c_str()), 0xffffff, 255, true,
                     m_textShadow ? 255 : 0, 0x000000, 0, 1, 1);
        tl->Update();
        m_textLineAdvance += chars * m_fontSize;
        if (!appendToCurrent)
            m_textCursorY += m_lineSpacing;
        pos = end;
    }
    if (!appendToCurrent)
        m_textLineAdvance = 0;
}

void OnsVM::ClearText()
{
    if (!m_screen || !m_screen->GetTextLayer())
        return;
    tTJSNI_BaseLayer* tl = m_screen->GetTextLayer();
    // 清整个 MainImage 区域（含窗背图右段）：m_textW/m_textH 只覆盖文本区，
    // 窗背（画在文本层、宽=屏宽）的 m_textW..层宽 段会残留（动画 leave 擦除
    // 后右下角灰块 = 该残留）。简单模型下窗背与文字同层，全清后由下一句
    // DrawTextPage 重画窗背。
    tTVPBaseTexture* img = tl->GetMainImage();
    int w = img ? (tjs_int)img->GetWidth() : (tjs_int)tl->GetWidth();
    int h = img ? (tjs_int)img->GetHeight() : (tjs_int)tl->GetHeight();
    void* buf = tl->GetMainImagePixelBufferForWrite();
    int pitch = tl->GetMainImagePixelBufferPitch();
    if (buf && pitch > 0)
    {
        // 直接清零层缓冲（ARGB=0 全透明；GPU Fill 对 alpha=0 跳过，CPU 直写可靠）
        for (int yy = 0; yy < h; yy++)
            memset((char*)buf + (size_t)yy * pitch, 0, (size_t)w * 4);
    }
    else
    {
        tl->FillRect(tTVPRect(0, 0, w, h), 0);
    }
    tl->Update();
    tl->Update();
    // 注意：不在此重置文字光标 —— ClearText 会在行中途被调用（页满换页），
    // 重置会把窗背内文字起点丢掉；每页入口 DrawTextPage 负责设光标。
    // 清屏后窗背也没了：下个画页需重载（m_textPageFresh）
    m_textPageFresh = true;
}

// 文本显示模式（上游 ONScripter_text.cpp enterTextDisplayMode/leaveTextDisplayMode）。
// 本项目窗背与文字同层（DrawTextPage 每次画页前画窗背），模式切换只需：
//  进入：置 TEXT 标志（窗背由下一页 DrawTextPage 画出）；
//  离开：erase_text_window_mode != 0 或 force 时清空文本层（窗背+文字一起消失）。
void OnsVM::EnterTextDisplayMode()
{
    if (!m_textMode)
    {
        m_textMode = true;
        // 上游在此处按 pretextgosub/saveon 条件 storeSaveFile（自动存档）。
        // 本项目存档仅由 savegame/rmenu-save 显式触发，不随页自动写档（见 docs/07 §5）。
    }
}

void OnsVM::LeaveTextDisplayMode(bool force)
{
    if (m_textMode && (force || m_eraseTextWindow != 0))
    {
        fprintf(stderr, "[ONS] -- leaveTextMode force=%d erase=%d\n", (int)force,
                m_eraseTextWindow);
        m_textMode = false;
        ClearText();
    }
}

// ---- lookback 回想（SYSTEM_LOOKBACK）：文本历史回看 ----
// 历史在 DrawTextPage 记录（净化文本逐句）；回看把最近若干句画到文本层
// （窗背之上），↑/↓ 翻页，点击/ESC 退出（退出后由脚本 goto *text_lb 恢复）。
void OnsVM::DrawLookback()
{
    if (!m_screen || !m_screen->GetTextLayer())
        return;
    if (!m_windowImage.empty() && !m_windowColorMode)
        m_screen->SetTextLayerWindowImage(OnsResolveStorageName(m_windowImage), m_winX,
                                          m_winY);
    else
        m_screen->SetTextLayerWindowImage("", 0, 0);
    int rows = std::max(1, m_textH / std::max(1, m_lineSpacing));
    int x0 = (m_textX - m_winX), y0 = (m_textY - m_winY);
    if (m_windowImage.empty())
        x0 = y0 = 0;
    tTJSNI_BaseLayer* tl = m_screen->GetTextLayer();
    int idx = m_lookbackIdx;
    if (idx < 0)
        idx = 0;
    for (int i = 0; i < rows && idx + i < (int)m_textHistory.size(); i++)
        tl->DrawText(x0, y0 + i * m_lineSpacing, ttstr(m_textHistory[idx + i].c_str()),
                     0xffffff, 255, true, 0, 0, 0, 0, 0);
    tl->Update();
}

void OnsVM::LookbackPage(int dir)
{
    int rows = std::max(1, m_textH / std::max(1, m_lineSpacing));
    int idx = m_lookbackIdx + dir * rows;
    if (idx < 0)
        idx = 0;
    if (idx > (int)m_textHistory.size())
        idx = (int)m_textHistory.size();
    m_lookbackIdx = idx;
    DrawLookback();
}

void OnsVM::ExitLookback()
{
    m_wait = WM_NONE;
    // 恢复文本层为窗背（当前句文字已随回看覆盖 —— 下一句文本重画时恢复）
    if (m_screen && m_screen->GetTextLayer())
    {
        if (!m_windowImage.empty() || m_windowColorMode)
            m_screen->SetTextLayerWindowImage(
                m_windowImage.empty() ? "" : OnsResolveStorageName(m_windowImage), m_winX,
                m_winY);
        else
            m_screen->SetTextLayerWindowImage("", 0, 0);
    }
}

// 回标题画面重置（goto *start / reset 命令共用）：清文本窗（对话框残留：
// rmenu 期间 erasetextwindow 0 令普通 leave 不擦）、清按钮链/悬停恢复串、
// 停快进与 windowerase 待恢复状态。
void OnsVM::ResetToStart()
{
    LeaveTextDisplayMode(true);
    HideBtnSprites();
    m_buttons.clear();
    m_erasePending = false;
    m_autoMode = m_skipMode = false;
    m_ctrlHeld = false;
}

// btn 按钮精灵隐藏：btn 命令画的按钮（BTN_BASE+id 精灵层）只在
// btnwait/textbtnwait 等待期间显示；等待返回/链重建时隐藏
// （上游：btnwaitCommand 尾部把全部按钮 show_flag 清 0，按钮不再被
// refreshSurface 绘制 —— 按钮不常驻，残留 = 没按此语义隐藏）。
void OnsVM::HideBtnSprites()
{
    if (m_screen && !m_btnSpriteIds.empty())
    {
        fprintf(stderr, "[ONS] -- hide %zu btn sprites\n", m_btnSpriteIds.size());
        for (int id : m_btnSpriteIds)
            m_screen->ClearSprite(id);
    }
    m_btnSpriteIds.clear();
    m_hoverButton = -1; // 链清理：悬停状态复位（防旧 exbtn_d 恢复串复活按钮）
    // exbtn_d 恢复串随链失效：它只属于当前按钮链（上游 exbtn_d 的悬停离开
    // 恢复只在按钮链存在时有意义）。菜单关闭（csp 清精灵 + 链重建）后若不
    // 清掉，故事页悬停 ▲/▼（btn 100/101 链）再离开时会执行旧恢复串
    // （如 r_menu 的 P21-30C11-20），把已清除的菜单按钮精灵复活 ——
    // "hover 回退按钮时 rmenu 跑出来"的根因。
    m_exbtnD.clear();
}

// rmenu 绘制（对齐上游 executeSystemMenu + 原版观感）：
// 半透明黑遮罩（菜单层 SetOpacity）盖全屏凸显文字；菜单项 = 文字精灵
// （TRANS_STRING 语义，cell0=普通 off 色 / cell1=悬停 on 色，与 CLANNAD
// 选项悬停同机制）；菜单块居中。
void OnsVM::DrawRMenu()
{
    if (!m_screen || m_rmenuItems.empty())
        return;
    int n = (int)m_rmenuItems.size();
    // 清上次菜单文字精灵（主菜单 <-> 槽列表切换时项数不同，多余项会残留）
    for (int i = 0; i < RMENU_TEXT_BASE_MAX; i++)
        m_screen->ClearSprite(RMENU_TEXT_BASE + i);

    // ---- 遮罩：菜单层整层半透明黑（Fill 0 = ARGB0 全透明，须带 alpha）----
    tTJSNI_BaseLayer* ml = m_screen->GetMenuLayer();
    if (ml)
    {
        ml->SetPosition(0, 0);
        // 层序提到文本/立绘/按钮之上（新建层 OrderIndex=0 会被 relayout 过
        // 的层盖住，遮罩/菜单文字画不出 —— rmenu 层序与 relayout 管理的
        // 游戏层分离：遮罩 100、文字 101）
        ml->SetOrderIndex(100);
        ml->FillRect(tTVPRect(0, 0, ml->GetWidth(), ml->GetHeight()), 0x96000000);
        ml->SetVisible(true);
        ml->Update();
    }

    // ---- 文字精灵 ----
    int menuW = 0;
    for (auto& it : m_rmenuItems)
    {
        int w = 6; // 边距
        for (size_t i = 0; i < it.label.size();)
        {
            unsigned char c = (unsigned char)it.label[i];
            if (c < 0x80)
            {
                w += m_menuFontSize * 55 / 100;
                i += 1;
            }
            else
            {
                i += (c < 0xE0) ? 2 : ((c < 0xF0) ? 3 : 4);
                w += m_menuFontSize;
            }
        }
        if (w > menuW)
            menuW = w;
    }
    int menuH = n * m_menuPitch;
    int bx = std::max(0, (m_screenW - menuW) / 2);
    int by = std::max(0, (m_screenH - menuH) / 2);

    m_rmenuRects.clear();
    for (int i = 0; i < n; i++)
    {
        std::vector<uint32_t> colors;
        colors.push_back(m_menuOffColor); // cell0 = 普通
        colors.push_back(m_menuOnColor);  // cell1 = 悬停
        m_screen->SetSpriteTextCells(RMENU_TEXT_BASE + i, m_rmenuItems[i].label,
                                     m_menuFontSize, colors);
        m_screen->SetSpritePos(RMENU_TEXT_BASE + i, bx, by + i * m_menuPitch);
        m_screen->SetSpriteVisible(RMENU_TEXT_BASE + i, true);
        m_rmenuRects.push_back(bx);
        m_rmenuRects.push_back(by + i * m_menuPitch);
        m_rmenuRects.push_back(menuW);
        m_rmenuRects.push_back(m_menuPitch);
    }
    // EnsureSpriteLayer 创建期会 RelayoutSprites（把新精灵按 z 排进 bg/fg
    // 段）——全部项创建完后统一把层序提到遮罩(100)之上，否则 rmenu 文字
    // 被排到背景段、画在游戏层之下不可见。
    for (int i = 0; i < n; i++)
    {
        tTJSNI_BaseLayer* l = m_screen->GetSpriteLayer(RMENU_TEXT_BASE + i);
        if (l)
        {
            l->SetOrderIndex(102);
            l->Update();
        }
    }
    m_rmenuHover = -1;
    m_rmenuVisible = true;
}

// 更新悬停项高亮（鼠标移动时）
void OnsVM::UpdateRMenuHover(int x, int y)
{
    if (!m_screen || !m_rmenuVisible)
        return;
    int hover = -1;
    for (int i = 0; i < (int)m_rmenuItems.size(); i++)
    {
        int rx = m_rmenuRects[i * 4];
        int ry = m_rmenuRects[i * 4 + 1];
        int rw = m_rmenuRects[i * 4 + 2];
        int rh = m_rmenuRects[i * 4 + 3];
        if (x >= rx && x < rx + rw && y >= ry && y < ry + rh)
        {
            hover = i;
            break;
        }
    }
    if (hover == m_rmenuHover)
        return;
    m_rmenuHover = hover;
    for (int i = 0; i < (int)m_rmenuItems.size(); i++)
        m_screen->SetSpriteCell(RMENU_TEXT_BASE + i, (i == m_rmenuHover) ? 1 : 0);
}

void OnsVM::CloseRMenu()
{
    if (!m_screen)
        return;
    tTJSNI_BaseLayer* ml = m_screen->GetMenuLayer();
    if (ml)
    {
        ml->SetVisible(false);
        ml->Update();
    }
    for (int i = 0; i < (int)m_rmenuItems.size(); i++)
        m_screen->ClearSprite(RMENU_TEXT_BASE + i);
    m_rmenuVisible = false;
    m_rmenuHover = -1;
    m_rmenuRects.clear();
    m_rmenuSlotMode = false;
}

// 槽位列表（rmenu 储存/读取 的选槽步；对齐上游 executeSystemSave/Load）
void OnsVM::OpenRMenuSlots(bool saving)
{
    std::vector<RMenuAction> slots;
    if (saving)
    {
        // 保存：全部槽位（已占用显示时间；覆盖时由保存直接覆盖）
        for (int s = 0; s < RMENU_SLOTS; s++)
        {
            RMenuAction a;
            a.label = SlotLabel(s, false);
            a.action = "save:" + std::to_string(s);
            slots.push_back(a);
        }
    }
    else
    {
        // 读取：只列有档槽；无档 -> 菜单只剩"取消"（点击=取消）
        bool any = false;
        for (int s = 0; s < RMENU_SLOTS; s++)
        {
            std::ifstream f(SaveSlotName(m_saveDir, s));
            if (!f)
                continue;
            any = true;
            RMenuAction a;
            a.label = SlotLabel(s, true);
            a.action = "load:" + std::to_string(s);
            slots.push_back(a);
        }
        if (!any)
        {
            RMenuAction a;
            a.label = std::string("没有存档");
            a.action = "cancel";
            slots.push_back(a);
        }
    }
    m_rmenuItems = slots;
    m_rmenuSlotMode = true;
    m_rmenuSlotSaving = saving;
    DrawRMenu();
}

// 槽位行标签："槽 N" + 有档时的时间（读取档首行 time <ts>）
std::string OnsVM::SlotLabel(int slot, bool withTime)
{
    std::string label = "保存槽 " + std::to_string(slot + 1);
    if (!withTime)
    {
        std::ifstream f(SaveSlotName(m_saveDir, slot));
        if (f)
            label += "（已有存档）";
        return label;
    }
    std::ifstream f(SaveSlotName(m_saveDir, slot));
    if (!f)
        return label;
    std::string magic;
    int ver = 0;
    f >> magic >> ver;
    std::string key;
    long long ts = 0;
    while (f >> key)
    {
        if (key == "time")
        {
            f >> ts;
            break;
        }
        if (key == "pc" || key == "callstack" || key == "vars" || key == "strvars" ||
            key == "sprites" || key == "textwin")
        {
            std::string line;
            std::getline(f, line);
        }
    }
    if (ts > 0)
    {
        time_t t = (time_t)ts;
        struct tm tmv;
        localtime_r(&t, &tmv);
        char buf[32];
        snprintf(buf, sizeof(buf), "  %04d/%02d/%02d %02d:%02d", tmv.tm_year + 1900,
                 tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
        label += buf;
    }
    return label;
}

// rmenu 动作执行（对齐上游 getSystemCallNo 映射；save/load 经槽位子菜单）。
// 返回：0 = 完成（关闭菜单、恢复打开前等待）；1 = 完成且立即继续脚本
// （读档/reset 已改 pc；不再恢复等待）；2 = 已切到子菜单（保持菜单继续等）。
int OnsVM::ExecRMenuAction(const std::string& action)
{
    if (action == "save")
    {
        // 上游 SYSTEM_SAVE：槽位选择
        OpenRMenuSlots(true);
        return 2;
    }
    if (action == "load")
    {
        // 上游 SYSTEM_LOAD：槽位选择（无档时 OpenRMenuSlots 给"没有存档"）
        OpenRMenuSlots(false);
        return 2;
    }
    if (action == "cancel")
        return 0;
    if (action.rfind("save:", 0) == 0)
    {
        int slot = atoi(action.c_str() + 5);
        if (slot >= 0 && slot < RMENU_SLOTS)
        {
            SaveState(slot);
            fprintf(stderr, "[ONS] -- rmenu save -> slot %d\n", slot);
        }
        return 0;
    }
    if (action.rfind("load:", 0) == 0)
    {
        int slot = atoi(action.c_str() + 5);
        if (slot >= 0 && slot < RMENU_SLOTS && LoadState(slot))
        {
            fprintf(stderr, "[ONS] -- rmenu load <- slot %d\n", slot);
            return 1; // pc 已跳：立即继续脚本
        }
        // 读档失败：回到槽列表
        OpenRMenuSlots(false);
        return 2;
    }
    if (action == "end")
    {
        m_finished = true;
        return 1;
    }
    if (action == "reset")
    {
        // 同 resetCommand：globalon 时保留全局变量（%911 等）
        if (!m_globalon)
            m_vars.clear();
        m_callStack.clear();
        m_loops.clear();
        HideBtnSprites(); // 旧按钮精灵一并隐藏（画面将由 *start 重绘）
        m_buttons.clear();
        auto it = m_labels.find("start");
        if (it != m_labels.end())
            m_pc = it->second - 1;
        return 1;
    }
    if (action == "skip")
    {
        // SYSTEM_SKIP（rm:240 executeSystemSkip）：skip_mode |= SKIP_NORMAL。
        // 关闭菜单回原文本/按钮等待后由 Step 的自动推进逻辑立即接管
        // （文本等待自动通过；任意真实点击停止 —— 与 Ctrl 快进同链）
        m_skipMode = true;
        m_autoMode = false;
        m_waitStart = OnsGetTicks();
        return 0;
    }
    if (action == "automode")
    {
        // SYSTEM_AUTOMODE（rm:246）：定时自动推进
        m_autoMode = true;
        m_skipMode = false;
        m_waitStart = OnsGetTicks();
        return 0;
    }
    if (action == "windowerase")
    {
        // SYSTEM_WINDOWERASE（rm:276 executeWindowErase）：隐藏文本框
        // （display_mode NORMAL），任意点击/按键恢复（WM_ERASE 分支）
        if (m_screen)
        {
            // 隐藏整个文本框：窗背层（windowLayer）+ 文字层（textLayer）
            tTJSNI_BaseLayer* wl = m_screen->GetWindowLayer();
            if (wl)
            {
                wl->SetVisible(false);
                wl->Update();
            }
            tTJSNI_BaseLayer* tl = m_screen->GetTextLayer();
            if (tl)
            {
                tl->SetVisible(false);
                tl->Update();
            }
        }
        m_erasePending = true;
        m_erasePrevWait = (int)m_rmenuPrevWait; // 恢复后回原文本/按钮等待
        m_wait = WM_ERASE;
        m_waitStart = OnsGetTicks();
        fprintf(stderr, "[ONS] -- rmenu windowerase: window+text hidden, WM_ERASE\n");
        return 1;
    }
    if (action == "lookback")
    {
        // SYSTEM_LOOKBACK（rm:625 executeSystemLookback）：回看文本历史
        if (m_textHistory.empty())
            return 0; // 无历史：仅关闭菜单
        m_lookbackIdx = (int)m_textHistory.size();
        LookbackPage(-1); // 定位到最近一页（显示末尾 rows 句）
        m_wait = WM_LOOKBACK;
        return 1;
    }
    // rmenu（嵌套菜单）等：暂为空操作
    return 0;
}

bool OnsVM::PollWaitEvent(OnsEvent* ev)
{
    // 陈旧输入过滤：m_waitStart 之前（>STALE_INPUT_MS）入队的点击/按键视为
    // 上一上下文（菜单按钮连点、画面切换前）的残留，丢弃 —— 防止它们在
    // 新等待中被逐个消费造成"一下子推进好多/误触新页面"。
    while (OnsPollEvent(ev))
    {
        // F12：调试全屏快照（不进入游戏输入流）
        if (ev->type == ONS_EVENT_KEYDOWN && ev->u.key.keysym.sym == ONSK_F12)
        {
            DebugFullShot();
            continue;
        }
        if (m_waitStart != 0 &&
            (ev->type == ONS_EVENT_MOUSEBUTTONDOWN || ev->type == ONS_EVENT_KEYDOWN ||
             ev->type == ONS_EVENT_MOUSEWHEEL) &&
            ev->timestamp + STALE_INPUT_MS < m_waitStart)
        {
            fprintf(stderr, "[ONS] -- drop stale input ts=%u waitStart=%u\n",
                    (unsigned)ev->timestamp, (unsigned)m_waitStart);
            continue;
        }
        return true;
    }
    return false;
}

void OnsVM::WaitForClick()
{
    m_waitStart = OnsGetTicks();
    m_wait = WM_CLICK;
}

int OnsVM::ClassifyButton(tjs_int tjsButton, int x, int y)
{
    // 按钮点击分类：1=左 2=中 3=右（ONSEngine 已转 ONS 键值）
    if (tjsButton == 3)
        return -1; // 右键（rmenu）
    if (tjsButton == 2)
        return 2; // 中键
    // 命中容差：ONScripter 按钮条常仅 14px 高，真实鼠标点击易偏出 1-2px；
    // 在 select_rect 上下各放宽 6px（仅命中测试，不影响 select_rect 本身）
    const int HIT_TOLERANCE = 6;
    for (auto& b : m_buttons)
    {
        if (x >= b.x && x < b.x + b.w && y >= b.y - HIT_TOLERANCE &&
            y < b.y + b.h + HIT_TOLERANCE)
        {
            fprintf(stderr, "[ONS] -- classify btn=%d at (%d,%d) hit id=%d rect=(%d,%d %dx%d)\n",
                   (int)tjsButton, x, y, b.id, b.x, b.y, b.w, b.h);
            return b.id;
        }
    }
    fprintf(stderr, "[ONS] -- classify btn=%d at (%d,%d) -> 0 (buttons=%zu)\n", (int)tjsButton, x, y, m_buttons.size());
    return 0; // 普通左键
}

// 文本 \ 标签解码：返回绘制段（\n 分段）；eol = 行尾是否 '\'（点击后不换页）；
// atWaits[k] = 段 k 后是否有裸 @（点击停顿后继续显示下一段，上游 clickWait）
void DecodeTextTags(const std::string& in, std::vector<std::string>& outLines,
                    std::vector<bool>& atWaits, bool& eol)
{
    outLines.clear();
    atWaits.clear();
    eol = false;
    std::string cur;
    size_t i = 0;
    while (i < in.size())
    {
        char c = in[i];
        if (c != '\\')
        {
            if (c == '@')
            {
                // 裸 @：NScr 点击停顿（clickWait）—— 当前段结束，标记等待
                outLines.push_back(cur);
                atWaits.push_back(true);
                cur.clear();
                i++;
                continue;
            }
            if (c == '[')
            {
                // 裸 [...]：NScr 行首/行内标签（说话人/语音等，不显示；
                // 上游 textCommand 解析 [..] 为 tag 并跳过）。\[ 才是字面。
                size_t close = in.find(']', i + 1);
                if (close != std::string::npos)
                {
                    i = close + 1;
                    continue;
                }
            }
            cur += c;
            i++;
            continue;
        }
        if (i + 1 >= in.size())
        {
            eol = true; // 行尾 '\'：改行待ち
            i++;
            break;
        }
        char n = in[i + 1];
        switch (n)
        {
            case 'n': // 换行
                outLines.push_back(cur);
                atWaits.push_back(false);
                cur.clear();
                i += 2;
                break;
            case 'r': // 换页（近似按换行处理）
                outLines.push_back(cur);
                atWaits.push_back(false);
                cur.clear();
                i += 2;
                break;
            case '@': // \@：字面 @（转义）
                cur += '@';
                i += 2;
                break;
            case '\\':
                cur += '\\';
                i += 2;
                break;
            case '%':
                cur += '%';
                i += 2;
                break;
            case 't': // 制表/跳格：忽略
                i += 2;
                break;
            case 's': // \s[...] 声音/字号：剥到 ]
                i += 2;
                if (i < in.size() && in[i] == '[')
                {
                    while (i < in.size() && in[i] != ']')
                        i++;
                    if (i < in.size())
                        i++;
                }
                break;
            case '[': // \[...] 转义序列：剥到 ]
                i += 2;
                while (i < in.size() && in[i] != ']')
                    i++;
                if (i < in.size())
                    i++;
                break;
            default:
                cur += n; // 未知标签：保留字符
                i += 2;
                break;
        }
    }
    if (!cur.empty() || outLines.empty())
    {
        outLines.push_back(cur);
        atWaits.push_back(false);
    }
}

bool OnsVM::DrawTextPage(const std::string& text)
{
    if (!m_screen || !m_screen->GetTextLayer())
        return true;

    EnterTextDisplayMode();

    // 窗口背景生命周期（上游 enterTextDisplayMode/leaveTextDisplayMode）：
    // 每次进入文本模式重绘窗背；页断点击（ClearText）后隐藏，下一段文本重新显示
    if (!m_windowImage.empty() || m_windowColorMode)
        m_screen->ShowTextWindow();

    std::string s = SubstituteVars(text);
    // 去尾空白后判定行尾标记
    std::string trimmed = s;
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t'))
        trimmed.pop_back();

    std::vector<std::string> segs;
    std::vector<bool> atWaits;
    bool eol = false;
    DecodeTextTags(trimmed, segs, atWaits, eol);

    // 窗背直接画到文本层（LoadImages 同时清掉旧文字 + 画出窗背）。
    // 页首（m_textPageFresh：页断点击/ClearText/换窗背之后）才重载窗背并定位
    // 首行；同页续行（名字行【武】+ 内容行、跨行句子）保留层内容叠加，
    // 光标随行尾推进 —— 上游 processText 缓冲续行语义。
    // 文字坐标 = 窗口位置相对窗背位置的偏移（窗背通常比文本窗口大）。
    if (m_textPageFresh)
    {
        if (m_windowColorMode)
            m_screen->SetTextLayerWindowImage("", 0, 0); // 纯色窗背暂用清层
        else if (!m_windowImage.empty())
            m_screen->SetTextLayerWindowImage(OnsResolveStorageName(m_windowImage), m_winX,
                                              m_winY);
        else
            ClearText(); // 无窗背：清层；文字从层内 (0,0) 起
        m_textCursorX = (m_windowImage.empty() && !m_windowColorMode) ? 0 : m_textX - m_winX;
        m_textCursorY = (m_windowImage.empty() && !m_windowColorMode) ? 0 : m_textY - m_winY;
        m_textPageFresh = false;
    }

    fprintf(stderr, "[ONS] -- drawpage win='%s' cur=(%d,%d) winxy=(%d,%d)\n",
            m_windowImage.c_str(), m_textCursorX, m_textCursorY, m_winX, m_winY);
    // 逐段绘制；裸 @（点击停顿）分段：画到 @ 前等待点击，点击后继续画剩余段
    // （上游 clickWait；@ 等待不清文本；@ 后段接续同行不换行）
    for (size_t k = 0; k < segs.size(); k++)
    {
        bool append = (k > 0 && atWaits[k - 1]); // @ 后的段接续当前行
        DrawTextLine(segs[k], append);
        if (atWaits[k] && k + 1 < segs.size())
        {
            m_pendingTextSegs.assign(segs.begin() + (long)k + 1, segs.end());
            m_pendingAtWaits.assign(atWaits.begin() + (long)k + 1, atWaits.end());
            m_pendingTextEol = eol;
            m_waitStart = OnsGetTicks();
            m_wait = WM_TEXTCLICK;
            m_textClickClear = false; // @ 等待：保留已画文本
            return false;
        }
    }
    if (segs.empty())
        m_textCursorY += m_lineSpacing; // 空文本行也占行

    // 回想历史：记录本句净化文本（标签已剥的分段拼接；限长防膨胀）
    if (!segs.empty())
    {
        std::string hist;
        for (auto& sg : segs)
            hist += sg;
        while (!hist.empty() && (hist.back() == ' ' || hist.back() == '\t'))
            hist.pop_back();
        if (!hist.empty())
        {
            m_textHistory.push_back(hist);
            if (m_textHistory.size() > 400)
                m_textHistory.erase(m_textHistory.begin());
        }
    }

    return FinishTextLine(eol);
}

// 文本行尾处理：textgosub（\ EOL 立即 gosub 例程，页断设 m_pageBreakPending）
// 或默认 WM_TEXTCLICK 点击推进（EOL 保留文本、页断清文本）
bool OnsVM::FinishTextLine(bool eol)
{
    if (!m_textGosubLabel.empty())
    {
        // textgosub 模式：行尾（\ EOL）立即 gosub（点击等待由例程内 textbtnwait
        // 提供，上游 clickWait/clickNewPage 的 textgosub 分支）。
        // 页断（\r / 无 \ 行尾）时：例程返回后要清文本（上游 newPage 清文本缓冲，
        // 窗背保持）—— 用 m_pageBreakPending 延迟到例程返回后的下一行执行前。
        if (!eol)
            m_pageBreakPending = true;
        auto it = m_labels.find(ToLower(m_textGosubLabel));
        if (it != m_labels.end())
        {
            m_callStack.push_back(m_pc);
            m_pc = it->second - 1;
        }
        return true;
    }

    // 默认（无 textgosub）—— 上游 \ = clickNewPage、行尾无标记 = 缓冲续行：
    //   '' 行尾：页断 —— 等待点击，点击后清文本换新页（上游 \ -> clickNewPage；
    //   ever17 一句（名字行+内容行）以内容行的 \ 收尾，点击进下一句）。
    //   行尾无 '\'：文本续行不等待（名字行【武】、跨行句子与内容同屏显示）。
    if (eol)
    {
        m_waitStart = OnsGetTicks();
        m_wait = WM_TEXTCLICK;
        m_textClickClear = true; // 点击后 ClearText（清页；fresh 重载窗背）
        return false;
    }
    return true;
}

void OnsVM::WaitForTicks(uint32_t ms)
{
    m_wait = WM_TIMER;
    m_waitUntil = OnsGetTicks() + ms;
}

void OnsVM::DumpLines(int from, int count)
{
    if (from < 0)
        from = 0;
    for (int i = from; i < from + count && i < (int)m_lines.size(); i++)
        fprintf(stderr, "[DUMP] %d: %s\n", i, m_lines[i].c_str());
}

bool OnsVM::DumpScript(const std::string& fileName)
{
    try
    {
        // 相对路径按存档目录解析（进程 CWD 不一定是游戏目录）
        std::string full = fileName;
        if (!full.empty() && full[0] != '/' && full[0] != '\\' &&
            !(full.size() > 1 && full[1] == ':') && !m_saveDir.empty())
            full = m_saveDir + fileName;
        std::ofstream f(full, std::ios::trunc);
        if (!f)
            return false;
        for (size_t i = 0; i < m_lines.size(); i++)
            f << i << "\t" << m_lines[i] << "\n";
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 把 setwindow 解析出的窗口参数应用到文本层（screen 未就绪时静默，
// 由 start() 在 CreateTextLayer 后再次调用）。窗背（m_windowImage）不在
// 这里画 —— DrawTextPage 每次画文本前画到文本层（简单模型）。
void OnsVM::ApplyTextWindow()
{
    if (!m_screen || !m_screen->GetTextLayer())
        return;
    tTJSNI_BaseLayer* tl = m_screen->GetTextLayer();
    tl->SetPosition(m_textX, m_textY);
    tl->SetSize((tjs_uint)m_textW, (tjs_uint)m_textH);
    if (!m_fontFile.empty())
    {
        tl->SetFontFace(ttstr(m_fontFile.c_str()));
        tl->SetFontFaceIsFileName(true);
    }
    tl->SetFontHeight(m_fontSize);
    tl->Update();
}

// ---------------------------------------------------------------------------
// 存档（槽位 n -> <savedir>/ons<n>.sav）：变量 + pc + 调用栈（自有格式）
// ---------------------------------------------------------------------------

// 24bpp BMP 写出（bottom-up；输入 BGRA 层内布局）
bool OnsVM::WriteShotBMP(const std::string& path, int w, int h,
                         const std::vector<uint32_t>& bgra)
{
    if (w <= 0 || h <= 0 || (int)bgra.size() < w * h)
        return false;
    try
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        int rowBytes = (w * 3 + 3) & ~3;
        uint32_t fileSize = 54 + (uint32_t)rowBytes * h;
        auto w16 = [&](uint16_t v) {
            f.put((char)(v & 0xff));
            f.put((char)((v >> 8) & 0xff));
        };
        auto w32 = [&](uint32_t v) {
            f.put((char)(v & 0xff));
            f.put((char)((v >> 8) & 0xff));
            f.put((char)((v >> 16) & 0xff));
            f.put((char)((v >> 24) & 0xff));
        };
        f.put('B');
        f.put('M');
        w32(fileSize);
        w32(0); // reserved
        w32(54); // offBits
        w32(40); // biSize
        w32((uint32_t)w);
        w32((uint32_t)h);
        w16(1);
        w16(24); // planes, bpp
        w32(0);  // BI_RGB
        w32((uint32_t)rowBytes * h);
        w32(0);
        w32(0); // resolution
        w32(0);
        w32(0); // colors
        std::vector<char> row((size_t)rowBytes, 0);
        for (int y = h - 1; y >= 0; y--)
        {
            const uint32_t* src = bgra.data() + (size_t)y * w;
            for (int x = 0; x < w; x++)
            {
                uint32_t p = src[x];
                row[(size_t)x * 3 + 0] = (char)(p & 0xff);        // B
                row[(size_t)x * 3 + 1] = (char)((p >> 8) & 0xff); // G
                row[(size_t)x * 3 + 2] = (char)((p >> 16) & 0xff); // R
            }
            f.write(row.data(), rowBytes);
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// 截图链路（savescreenshot 与调试钩子共享）：
// 优先用 getscreenshot 时的内存快照，无则现拍；最近邻缩放到 (w,h)；写 save_dir/file。
void OnsVM::DebugFullShot()
{
    if (!m_screen)
        return;
    int fw = 0, fh = 0;
    std::vector<uint32_t> buf;
    if (!m_screen->CompositeSnapshot(buf, fw, fh) || buf.empty() || fw <= 0 || fh <= 0)
        return;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/ons_dbg_%u.bmp", (unsigned)(OnsGetTicks() / 1000));
    WriteShotBMP(path, fw, fh, buf);
    fprintf(stderr, "[ONS] -- debug fullshot -> %s (%dx%d)\n", path, fw, fh);
}

void OnsVM::TakeScreenshot(int w, int h, const std::string& file)
{
    if (w <= 0)
        w = 160;
    if (h <= 0)
        h = 120;
    if (m_shotBuffer.empty() && m_screen)
    {
        int fw = 0, fh = 0;
        std::vector<uint32_t> buf;
        if (m_screen->CompositeSnapshot(buf, fw, fh))
        {
            m_shotBuffer.swap(buf);
            m_shotFullW = fw;
            m_shotFullH = fh;
        }
    }
    if (m_shotBuffer.empty() || m_shotFullW <= 0 || m_shotFullH <= 0)
        return;
    std::vector<uint32_t> scaled((size_t)w * h);
    for (int y = 0; y < h; y++)
    {
        int sy = std::min(m_shotFullH - 1, y * m_shotFullH / h);
        const uint32_t* srow = m_shotBuffer.data() + (size_t)sy * m_shotFullW;
        uint32_t* drow = scaled.data() + (size_t)y * w;
        for (int x = 0; x < w; x++)
            drow[x] = srow[std::min(m_shotFullW - 1, x * m_shotFullW / w)];
    }
    std::string path = m_saveDir + file;
    WriteShotBMP(path, w, h, scaled);
    fprintf(stderr, "[ONS] -- savescreenshot -> '%s' (%dx%d)\n", path.c_str(), w, h);
}

// 存档格式 v2（在 v1 逻辑状态之上保存画面状态；上游 saveAll/loadSaveFile
// 保存 bg_info/sprite_info/sentence_font_info/display_mode 的对应物）：
//   bg <原始规格行>          — bg 命令第一参数（file/white/black/矩形规格）
//   textwin ...（数值）+ winfile <行>
//   textmode 0|1
//   sprites N + 每精灵两行（数值行 + file <行>）
//   strvars N + 每项 name <值行>
void OnsVM::SaveState(int slot)
{
    try
    {
        std::string path = SaveSlotName(m_saveDir, slot);
        std::ofstream f(path, std::ios::trunc);
        if (!f)
            return;
        f << "KRKR_ONS_SAVE 3\n";
        f << "time " << (long long)time(nullptr) << "\n";
        f << "pc " << m_pc << "\n";
        f << "callstack";
        for (int v : m_callStack)
            f << " " << v;
        f << "\n";
        // BGM 状态（上游 saveAll 保存 music 状态；读档恢复标题/剧情音乐）
        if (m_audio)
            f << "bgm " << m_audio->GetCurrentBgm() << "\n";
        // 画面：bg / 文本窗 / 文本模式
        f << "bg " << m_bgSpec << "\n";
        f << "textwin " << m_textX << " " << m_textY << " " << m_textW << " "
          << m_textH << " " << m_fontSize << " " << m_lineSpacing << " "
          << (m_textShadow ? 1 : 0) << " " << (m_windowColorMode ? 1 : 0) << " "
          << m_windowColor << " " << m_winX << " " << m_winY << "\n";
        f << "winfile " << m_windowImage << "\n";
        f << "textmode " << (m_textMode ? 1 : 0) << "\n";
        // 精灵
        f << "sprites " << m_spriteState.size() << "\n";
        for (auto& kv : m_spriteState)
        {
            const SpriteState& st = kv.second;
            f << "sp " << kv.first << " " << st.x << " " << st.y << " " << st.alpha
              << " " << (st.visible ? 1 : 0) << " " << (st.textSprite ? 1 : 0) << " "
              << st.fontSize << " " << st.color << " " << st.mode << "\n";
            f << "spfile " << st.file << "\n";
        }
        // 变量 + 字符串变量
        f << "vars " << m_vars.size() << "\n";
        for (auto& kv : m_vars)
            f << kv.first << " " << kv.second << "\n";
        f << "strvars " << m_strVars.size() << "\n";
        for (auto& kv : m_strVars)
            f << kv.first << " " << kv.second << "\n";
    }
    catch (...)
    {
    }
    // 背景像素导出：ever17 大量背景由 blt/specialEffectImage 直绘主层，
    // m_bgSpec（最近一次 bg 文件名）恢复不了特效画面 —— 存档时把主层
    // 导出为 onsN_bg.bmp（与存档同目录），读档贴回 = 存档画面还原。
    // （SaveLayerImage/LoadImages 同体系，格式天然匹配）
    try
    {
        if (m_screen && m_screen->GetPrimaryLayer())
        {
            std::string dir = m_saveDir;
            if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
                dir += '/';
            m_screen->GetPrimaryLayer()->SaveLayerImage(
                ttstr((dir + "ons" + std::to_string(slot) + "_bg.bmp").c_str()), TJS_N("bmp"));
        }
    }
    catch (...)
    {
    }
}

// 读档后重建画面（bg + 精灵 + 文本窗/文本模式；上游 loadSaveFile 语义）
void OnsVM::RestoreVisualState()
{
    if (!m_screen)
        return;
    // 先清全部精灵：读档前的画面（标题菜单/读档页按钮等）不得残留，
    // 只恢复存档记录的画面（上游 loadSaveFile 后脚本从存档 pc 继续）
    m_screen->ClearSprite(-1);
    // bg：直接重放 bg 命令（无特效；内部同时做 leaveTextDisplayMode/清立绘）
    if (!m_bgSpec.empty())
    {
        std::vector<std::string> a{m_bgSpec};
        ExecuteCommand("bg", a);
    }
    // 精灵
    for (auto& kv : m_spriteState)
    {
        const SpriteState& st = kv.second;
        int n = kv.first;
        if (st.textSprite)
            m_screen->SetSpriteText(n, st.file, st.fontSize, st.color);
        else if (!st.file.empty())
        {
            // 读档恢复：优先用存档模式；旧档无记录 -> 按无前缀默认（transmode）
            int mode = st.mode;
            if (mode < 0)
            {
                std::string tmp;
                mode = ResolveImageSpec("", tmp, OnsScreen::IMG_ALPHA);
            }
            m_screen->SetSpriteImage(n, st.file, mode);
        }
        else
            continue;
        m_screen->SetSpritePos(n, st.x, st.y);
        if (st.alpha >= 0)
            m_screen->SetSpriteOpacity(n, st.alpha);
        m_screen->SetSpriteVisible(n, st.visible);
    }
    // 文本窗参数 + 文本模式（上游读档后 display_mode = TEXT，ONScripter_file2.cpp:449）
    if (m_screen->GetTextLayer())
    {
        ApplyTextWindow();
        // 重建文本层内容：档记窗背模式 -> 重画窗背（LoadImages 同时清旧文字）；
        // 无窗背 -> 清空文本层（读档前画面（读档页/菜单）残留的窗背与文字
        // 不得留在读档后的画面上 —— 上游 loadSaveFile2 的文本页缓冲整体重建）
        if (m_savedTextMode)
        {
            if (!m_windowImage.empty() || m_windowColorMode)
                m_screen->SetTextLayerWindowImage(
                    m_windowImage.empty() ? "" : OnsResolveStorageName(m_windowImage),
                    m_winX, m_winY);
            else
                m_screen->SetTextLayerWindowImage("", 0, 0);
        }
        else
            m_screen->SetTextLayerWindowImage("", 0, 0);
    }
    m_textMode = m_savedTextMode;
    // 强制整屏重绘一次：上游 loadgameCommand 成功路径 dirty_rect.fill + flush
    // （ONScripter_command.cpp:1871-88）——读档画面重建涉及几十层操作，任何
    // 增量标脏遗漏都会把读档前的旧层内容留在屏幕上（"读档后残留"根因）。
    m_screen->ForceFullRefresh();
}

bool OnsVM::LoadState(int slot)
{
    try
    {
        std::string path = SaveSlotName(m_saveDir, slot);
        std::ifstream f(path);
        fprintf(stderr, "[ONS] -- LoadState(%d) path='%s' %s\n", slot, path.c_str(),
                f ? "OK" : "NOT FOUND");
        if (!f)
            return false;
        std::string magic;
        int ver = 0;
        f >> magic >> ver;
        if (magic != "KRKR_ONS_SAVE" || (ver != 1 && ver != 2 && ver != 3))
            return false;
        std::string key;
        int pc = 0;
        while (f >> key)
        {
            if (key == "pc")
            {
                f >> pc;
            }
            else if (key == "callstack")
            {
                m_callStack.clear();
                int v;
                while (f.peek() != '\n' && f >> v)
                    m_callStack.push_back(v);
            }
            else if (key == "vars")
            {
                int n = 0;
                f >> n;
                m_vars.clear();
                for (int i = 0; i < n; i++)
                {
                    std::string name;
                    int val;
                    f >> name >> val;
                    m_vars[ResolveVarName(name)] = val; // 旧档 "%911" -> "911"
                }
            }
            else if (key == "strvars")
            {
                int n = 0;
                f >> n;
                m_strVars.clear();
                for (int i = 0; i < n; i++)
                {
                    std::string name, line, val;
                    f >> name;
                    std::getline(f, line); // 值行（可能含空格）
                    size_t p = line.find_first_not_of(' ');
                    if (p != std::string::npos)
                        val = line.substr(p);
                    m_strVars[name] = val;
                }
            }
            else if (key == "bgm")
            {
                // 按行读取（bgm 可能为空：f>> 会越过空行把下一行开头的 key
                // （如 "bg"）读成 BGM 名，键流从此错位 —— 曾致读档背景/后续
                // 全部键丢失，画面保持读档前旧背景）
                std::string line;
                std::getline(f, line);
                size_t p0 = line.find_first_not_of(' ');
                std::string bgm = (p0 == std::string::npos) ? "" : line.substr(p0);
                if (m_audio && !bgm.empty())
                    m_audio->PlayBgm(bgm);
            }
            else if (key == "bg" && ver >= 2)
            {
                std::string line;
                std::getline(f, line);
                size_t p = line.find_first_not_of(' ');
                m_bgSpec = (p == std::string::npos) ? "" : line.substr(p);
            }
            else if (key == "textwin" && ver >= 2)
            {
                int shadow = 0, colorMode = 0, color = 0;
                f >> m_textX >> m_textY >> m_textW >> m_textH >> m_fontSize >>
                    m_lineSpacing >> shadow >> colorMode >> color >> m_winX >> m_winY;
                m_textShadow = shadow != 0;
                m_windowColorMode = colorMode != 0;
                m_windowColor = (uint32_t)color;
            }
            else if (key == "winfile" && ver >= 2)
            {
                std::string line;
                std::getline(f, line);
                size_t p = line.find_first_not_of(' ');
                m_windowImage = (p == std::string::npos) ? "" : line.substr(p);
            }
            else if (key == "textmode" && ver >= 2)
            {
                int tm = 0;
                f >> tm;
                m_savedTextMode = tm != 0;
            }
            else if (key == "sprites" && ver >= 2)
            {
                int n = 0;
                f >> n;
                m_spriteState.clear();
                for (int i = 0; i < n; i++)
                {
                    std::string k2;
                    f >> k2; // "sp"
                    SpriteState st;
                    int idx = 0, vis = 0, isText = 0, color = 0, mode = -1;
                    f >> idx >> st.x >> st.y >> st.alpha >> vis >> isText >> st.fontSize >>
                        color;
                    if (ver >= 3)
                        f >> mode;
                    st.color = (uint32_t)color;
                    st.mode = mode;
                    st.textSprite = isText != 0;
                    st.visible = vis != 0;
                    f >> k2; // "spfile"
                    std::string line;
                    std::getline(f, line);
                    size_t p = line.find_first_not_of(' ');
                    st.file = (p == std::string::npos) ? "" : line.substr(p);
                    m_spriteState[idx] = st;
                }
            }
        }
        m_pc = pc;
        m_finished = false;
        m_wait = WM_NONE;
        // 读档后停 automode/skip（上游 loadgameCommand 成功路径同样复位）
        m_autoMode = m_skipMode = false;
        m_loops.clear();
        HideBtnSprites(); // 读档前隐藏旧按钮（画面随后由 RestoreVisualState 重建）
        m_buttons.clear();
        m_selectItems.clear();
        m_pendingStmts.clear();
        m_pendingIdx = 0;
        if (ver >= 2)
            RestoreVisualState();
        // 像素级背景：导出图（onsN_bg.bmp）存在则贴回主层 —— 覆盖 bg 重放
        // 结果（特效直绘背景 = 存档画面逐像素还原；旧档无导出图则跳过，
        // 保持文件名重放语义）
        {
            std::string dir = m_saveDir;
            if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
                dir += '/';
            std::string bgp = dir + "ons" + std::to_string(slot) + "_bg.bmp";
            std::ifstream chk(bgp.c_str());
            if (chk && m_screen && m_screen->GetPrimaryLayer())
            {
                try
                {
                    tTJSNI_BaseLayer* bg = m_screen->GetPrimaryLayer();
                    bg->LoadImages(ttstr(bgp.c_str()), 0x1FFFFFFF);
                    bg->Update();
                    fprintf(stderr, "[ONS] -- load bg pixels '%s'\n", bgp.c_str());
                }
                catch (...)
                {
                }
            }
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

// gloval.sav（上游 saveGlovalData：globalon 时把全部变量写入 gloval.sav；
// 读取：启动时 LoadGlovalState、definereset 时重读。本项目按用户要求
// “随时保存”：SetVar/SetStrVar 置脏，FlushGlovalState 去抖（>1s）落盘，
// stop 时强制落盘 —— 保证 %911 之类的标志跨启动持久。）
void OnsVM::SaveGlovalState()
{
    if (!m_globalon)
        return; // 上游 saveGlovalData 同样只在 globalon_flag 时写
    try
    {
        std::string path = m_saveDir + "gloval.sav";
        std::ofstream f(path, std::ios::trunc);
        if (!f)
            return;
        f << "KRKR_ONS_GLOVAL 1\n";
        f << "vars " << m_vars.size() << "\n";
        for (auto& kv : m_vars)
            f << kv.first << " " << kv.second << "\n";
        f << "strvars " << m_strVars.size() << "\n";
        for (auto& kv : m_strVars)
            f << kv.first << " " << kv.second << "\n";
        fprintf(stderr, "[ONS] -- gloval.sav saved (%zu vars)\n", m_vars.size());
    }
    catch (...)
    {
    }
}

void OnsVM::FlushGlovalState(bool force)
{
    if (!m_glovalDirty && !force)
        return;
    uint32_t now = OnsGetTicks();
    if (!force && now - m_glovalLastSave < 1000)
        return; // 去抖：频繁 mov 不落盘
    SaveGlovalState();
    m_glovalDirty = false;
    m_glovalLastSave = now;
}

bool OnsVM::LoadGlovalState()
{
    try
    {
        std::string path = m_saveDir + "gloval.sav";
        std::ifstream f(path);
        if (!f)
        {
            fprintf(stderr, "[ONS] -- gloval.sav not found (fresh start)\n");
            return false;
        }
        std::string magic;
        int ver = 0;
        f >> magic >> ver;
        if (magic != "KRKR_ONS_GLOVAL" || ver != 1)
            return false;
        std::string key;
        while (f >> key)
        {
            if (key == "vars")
            {
                int n = 0;
                f >> n;
                for (int i = 0; i < n; i++)
                {
                    std::string name;
                    int val;
                    f >> name >> val;
                    m_vars[ResolveVarName(name)] = val; // 旧档 "%911" -> "911"
                }
            }
            else if (key == "strvars")
            {
                int n = 0;
                f >> n;
                for (int i = 0; i < n; i++)
                {
                    std::string name, line;
                    f >> name;
                    std::getline(f, line);
                    size_t p = line.find_first_not_of(' ');
                    m_strVars[name] = (p == std::string::npos) ? "" : line.substr(p);
                }
            }
        }
        fprintf(stderr, "[ONS] -- gloval.sav loaded (%zu vars)\n", m_vars.size());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void OnsVM::DrawSelectMenu()
{
    if (!m_screen || !m_screen->GetTextLayer())
        return;
    tTJSNI_BaseLayer* tl = m_screen->GetTextLayer();
    int menuH = (int)m_selectItems.size() * m_lineSpacing;
    // 清除上次菜单区域（保留菜单上方正文）
    tl->FillRect(tTVPRect(0, m_selectMenuY, m_textW > 0 ? m_textW : tl->GetWidth(), menuH), 0);
    for (size_t i = 0; i < m_selectItems.size(); i++)
    {
        uint32_t color = ((int)i == m_selectCursor) ? 0xffff00 : 0xffffff;
        std::string line = ((int)i == m_selectCursor ? ">" : " ") + m_selectItems[i].text;
        tl->DrawText(m_textCursorX, m_selectMenuY + (int)i * m_lineSpacing, ttstr(line.c_str()),
                     color, 255, true, 0, 0, 0, 0, 0);
    }
    tl->Update();
}

void OnsVM::ConfirmSelect()
{
    // 选择支需用户输入：停 automode/skip（上游 selectCommand 关 automode_flag）
    m_autoMode = m_skipMode = false;
    m_wait = WM_NONE;
    if (m_selectMode == 1)
    {
        // selnum：选中序号（0 起）写入变量，继续执行下一行
        SetVar(m_selectVar, m_selectCursor);
    }
    else if (m_selectCursor >= 0 && m_selectCursor < (int)m_selectItems.size())
    {
        // select：跳到所选标签（标签行为空操作，从下一行开始执行）
        const std::string& lbl = m_selectItems[m_selectCursor].label;
        if (!lbl.empty())
        {
            auto it = m_labels.find(ToLower(lbl));
            if (it != m_labels.end())
                m_pc = it->second;
        }
    }
    ClearText();
}

bool OnsVM::GetVar(const std::string& name, int& value) const
{
    if (name.empty())
        return false;
    std::string n = ResolveVarName(name);
    auto it = m_vars.find(n);
    if (it != m_vars.end())
    {
        value = it->second;
        return true;
    }
    // 数字变量：未初始化按 0
    value = 0;
    return true;
}

void OnsVM::SetVar(const std::string& name, int value)
{
    if (name.empty())
        return;
    m_vars[ResolveVarName(name)] = value;
    MarkGlovalDirty(); // 随时保存（去抖落盘，见 FlushGlovalState）
}

bool OnsVM::GetStrVar(const std::string& name, std::string& value) const
{
    if (name.empty())
        return false;
    auto it = m_strVars.find(name);
    if (it == m_strVars.end())
    {
        value.clear();
        return true; // 未初始化按空串
    }
    value = it->second;
    return true;
}

void OnsVM::SetStrVar(const std::string& name, const std::string& value)
{
    if (name.empty())
        return;
    m_strVars[name] = value;
    MarkGlovalDirty(); // 随时保存（去抖落盘，见 FlushGlovalState）
}

std::string OnsVM::GetRawArg(const std::vector<std::string>& args, size_t i)
{
    return i < args.size() ? args[i] : "";
}

// 字符串表达式求值（$a+"lit"+$b 拼接；SubstituteVars + 引号字面）
std::string OnsVM::EvalStringExpr(const std::string& s)
{
    std::string out;
    std::string cur;
    bool inQuote = false;
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '"')
        {
            if (inQuote)
            {
                out += cur;
                cur.clear();
            }
            inQuote = !inQuote;
            continue;
        }
        if (c == '+' && !inQuote)
        {
            std::string seg = Trim(cur);
            cur.clear();
            if (!seg.empty())
                out += SubstituteVars(seg);
            continue;
        }
        cur += c;
    }
    std::string seg = Trim(cur);
    if (!seg.empty())
        out += SubstituteVars(seg);
    return out;
}

std::string OnsVM::SubstituteVars(const std::string& text) const
{
    std::string out;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == '%')
        {
            size_t close = text.find('%', i + 1);
            if (close != std::string::npos)
            {
                int v = 0;
                if (GetVar(text.substr(i + 1, close - i - 1), v))
                {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d", v);
                    out += buf;
                    i = close;
                    continue;
                }
            }
        }
        else if (text[i] == '$')
        {
            // $字符串变量（$0/$voice/...）：名 = 字母数字下划线
            size_t j = i + 1;
            while (j < text.size() &&
                   ((text[j] >= 'a' && text[j] <= 'z') || (text[j] >= 'A' && text[j] <= 'Z') ||
                    (text[j] >= '0' && text[j] <= '9') || text[j] == '_'))
                j++;
            if (j > i + 1)
            {
                std::string sv;
                if (GetStrVar(text.substr(i + 1, j - i - 1), sv))
                {
                    out += sv;
                    i = j - 1;
                    continue;
                }
                // 未定义 $var = 空串（对齐 ：vd.str 未赋值即 NULL → ""）
                i = j - 1;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

bool OnsVM::EvalExpression(const std::string& expr, int& result)
{
    ExprParser p(expr, this);
    return p.Parse(result);
}

// 执行 exbtn/exbtn_d 控制串（上游 decodeExbtnControl 语义）：
//   Cxx / Cxx-yy：隐藏 sprite xx..yy（refreshSprite false）
//   Pxx[,cell]：显示 sprite xx（refreshSprite true）
//   sN,(file)：播放音效到通道 N
void OnsVM::ExecExbtnControl(const std::string& ctl)
{
    if (ctl.empty() || !m_screen)
        return;
    size_t i = 0;
    while (i < ctl.size())
    {
        char com = ctl[i];
        if (com == 'C' || com == 'c' || com == 'P' || com == 'p')
        {
            i++;
            int n = 0;
            while (i < ctl.size() && ctl[i] >= '0' && ctl[i] <= '9')
                n = n * 10 + (ctl[i++] - '0');
            int n2 = n;
            if (i < ctl.size() && ctl[i] == '-')
            {
                i++;
                n2 = 0;
                while (i < ctl.size() && ctl[i] >= '0' && ctl[i] <= '9')
                    n2 = n2 * 10 + (ctl[i++] - '0');
            }
            for (int k = n; k <= n2; k++)
            {
                if (com == 'P' || com == 'p')
                    m_screen->SetSpriteVisible(k, true);
                else
                    m_screen->SetSpriteVisible(k, false);
                fprintf(stderr, "[ONS] -- exbtnctl %c%d -> %s\n", com, k,
                        (com == 'P' || com == 'p') ? "show" : "hide");
            }
        }
        else if (com == 's' || com == 'S')
        {
            i++;
            while (i < ctl.size() && ctl[i] >= '0' && ctl[i] <= '9')
                i++;
            if (i < ctl.size() && ctl[i] == ',')
                i++;
            if (i < ctl.size() && ctl[i] == '(')
            {
                i++;
                std::string file;
                while (i < ctl.size() && ctl[i] != ')' && ctl[i] != ',')
                    file += ctl[i++];
                if (m_audio && !file.empty())
                    m_audio->PlaySe(OnsResolveStorageName(file));
            }
        }
        else
        {
            i++;
        }
        // 跳过逗号/空格分隔
        while (i < ctl.size() && (ctl[i] == ',' || ctl[i] == ' '))
            i++;
    }
}

// 鼠标悬停更新（上游 mouseOverCheck）：悬停按钮变化时执行 exbtn 控制串
// （进入：C 隐藏普通图 + P 显示按下图 + 音效；离开：exbtn_d 恢复普通态）。
// 无控制串的 SPRITE_BUTTON（spbtn/上游 975-1016）：悬停进入/离开切换精灵
// cell 1/0 —— 字符串精灵颜色帧（如 CLANNAD 选项文字 #ffffff/#CCCCCC）。
void OnsVM::UpdateHover(int x, int y)
{
    // 无按钮链（菜单已关/链已删）：悬停状态复位。此时执行 exbtn_d 恢复串
    // （Pxx 显示）会把刚被 csp 清掉的按钮精灵重新显示 —— "菜单关闭后残留"
    // 的根因（上游 mouseOverCheck 只在按钮链上工作，链空无悬停语义）。
    if (m_buttons.empty())
    {
        m_hoverButton = -1;
        return;
    }
    int hit = -1;
    for (auto& b : m_buttons)
    {
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
        {
            hit = b.id;
            break;
        }
    }
    if (hit == m_hoverButton)
        return;
    fprintf(stderr, "[ONS] -- hover %d -> %d (exbtnD='%s')\n", m_hoverButton, hit,
            m_exbtnD.c_str());
    if (m_hoverButton != -1)
    {
        // 离开旧按钮：SPRITE_BUTTON 无控制串 -> cell 回 0（上游 975-983）；
        // 有 exbtn_d 则恢复串统一恢复普通态
        for (auto& b : m_buttons)
            if (b.id == m_hoverButton)
            {
                if (b.ctl.empty() && b.spriteNo >= 0 && m_screen)
                    m_screen->SetSpriteCell(b.spriteNo, 0);
                break;
            }
        if (!m_exbtnD.empty())
            ExecExbtnControl(m_exbtnD);
    }
    m_hoverButton = hit;
    if (hit != -1)
    {
        // 进入新按钮：exbtn 控制串优先（显示按下图等）；无控制串的
        // SPRITE_BUTTON -> cell 1（悬停高亮帧）
        for (auto& b : m_buttons)
        {
            if (b.id == hit)
            {
                if (!b.ctl.empty())
                    ExecExbtnControl(b.ctl);
                else if (b.spriteNo >= 0 && m_screen)
                    m_screen->SetSpriteCell(b.spriteNo, 1);
                break;
            }
        }
    }
}

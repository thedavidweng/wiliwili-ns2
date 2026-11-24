// Pure Switch 2 text decisions: the system keyboard buffer, the sensitive-input
// flag, the pinyin fallback for a runtime where the system keyboard will not
// start, and the shared-font fallback chain. Shared by the Borealis overrides
// and the host tests, so nothing here may include <switch.h>.
//
// The numeric shared-font types are PlSharedFontType from switch/services/pl.h;
// switch_font.cpp asserts them against the real header.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "utils/platform_api.h"

namespace wili::switch2 {

// The system keyboard counts characters, UTF-8 needs up to four bytes each, and
// the terminating NUL has to fit as well. A 0x100 buffer silently truncated
// long Chinese input.
inline constexpr std::size_t IME_BYTES_PER_CHARACTER = 4;

inline std::size_t imeBufferBytes(int max_string_length) {
    const std::size_t characters = max_string_length < 1 ? 1 : (std::size_t)max_string_length;
    return characters * IME_BYTES_PER_CHARACTER + 1;
}

// Masking is not one of Borealis' disable bits: KEYBOARD_DISABLE_SPACE and the
// other seven each forbid a character, and forbidding spaces never made a field
// a password. KEYBOARD_SENSITIVE_INPUT (utils/platform_api.h) is the separate
// request, outside that range.
inline bool imeIsSensitive(int kbd_disable_bitmask) { return (kbd_disable_bitmask & KEYBOARD_SENSITIVE_INPUT) != 0; }

// Borealis' own disable bits, with the platform-only flag removed: the mapping
// onto SwkbdKeyDisableBitmask must not see it as a character to forbid.
inline int imeKeyDisableBits(int kbd_disable_bitmask) { return kbd_disable_bitmask & ~KEYBOARD_SENSITIVE_INPUT; }

// One Pinyin syllable and its Hanzi candidates, most common first, UTF-8 and
// space separated. The table is authored for this port from common characters
// (no third-party dictionary), so the in-app fallback stays small and carries
// nothing that has to be redistributed under another licence.
struct PinyinSyllable {
    const char* syllable;
    const char* hanzi;
};

const std::vector<PinyinSyllable>& pinyinTable();

// Hanzi candidates for one Pinyin syllable, empty when the table does not know
// it. Traditional Chinese is not stored twice: the caller converts with the
// linked OpenCC s2t dictionary, which the rest of the UI already uses.
std::vector<std::string> pinyinCandidates(const std::string& syllable);

// Composition state of the swkbd-unavailable fallback: letters build one
// syllable, a space commits the most common candidate and 1..9 pick another.
// Nothing is logged, sensitive or not.
class PinyinComposer {
public:
    // Adds one ASCII letter. False when the character is not part of a syllable,
    // so the caller can treat it as ordinary text.
    bool append(char letter);

    const std::string& syllable() const { return this->buffer; }

    std::vector<std::string> candidates() const { return pinyinCandidates(this->buffer); }

    // Commits candidate `index` of the current syllable and clears it. Empty when
    // the buffer has no such candidate.
    std::string commit(std::size_t index = 0);

    // Drops the last letter of the syllable being composed.
    bool backspace();

    void clear() { this->buffer.clear(); }

private:
    std::string buffer;
};

// The longest syllable in the table (zhuang, chuang, shuang).
inline constexpr std::size_t PINYIN_MAX_SYLLABLE = 6;

inline const std::vector<PinyinSyllable>& pinyinTable() {
    static const std::vector<PinyinSyllable> table = {
        {"ai", "爱"},           {"an", "安"},           {"ba", "八 把 吧"},
        {"bai", "白 百"},       {"ban", "半 版"},       {"bang", "帮 榜 棒"},
        {"bao", "宝 报 包"},    {"bei", "北 被 背"},    {"ben", "本"},
        {"bi", "比 笔 必"},     {"biao", "表 标"},      {"bie", "别"},
        {"bing", "冰 病"},      {"bo", "播 波 博"},     {"bu", "不 部 步"},
        {"cai", "才 采"},       {"can", "参 餐"},       {"cang", "藏 仓"},
        {"ce", "测"},           {"cha", "查 茶"},       {"chang", "长 场 唱"},
        {"chao", "超 朝"},      {"che", "车"},          {"chen", "陈 晨"},
        {"cheng", "成 城 程"},  {"chi", "吃 持"},       {"chu", "出 初"},
        {"chuan", "传 船"},     {"chun", "春"},         {"ci", "次 词"},
        {"cong", "从"},         {"cun", "存 村"},       {"cuo", "错"},
        {"da", "大 打"},        {"dai", "带 代"},       {"dan", "单 但"},
        {"dang", "当"},         {"dao", "到 道 导"},    {"de", "的 得 德"},
        {"deng", "等 登"},      {"di", "第 地 低"},     {"dian", "电 点 店"},
        {"diao", "调 掉"},      {"ding", "定 顶"},      {"dong", "动 东 冬"},
        {"dou", "都 豆"},       {"du", "度 读 独"},     {"duan", "段 短"},
        {"dui", "对 队"},       {"duo", "多"},          {"er", "二 儿"},
        {"fa", "发 法"},        {"fan", "番 反 饭"},    {"fang", "方 房"},
        {"fei", "飞 非"},       {"fen", "分 粉 份"},    {"feng", "风 封"},
        {"fu", "服 复 福"},     {"gai", "改 该"},       {"gan", "感 干"},
        {"gang", "刚 钢"},      {"gao", "高 告"},       {"ge", "个 歌 各"},
        {"gei", "给"},          {"gen", "跟"},          {"geng", "更"},
        {"gong", "工 公 共"},   {"gou", "够 狗"},       {"gu", "古 故 股"},
        {"guan", "关 观 官"},   {"guang", "光 广"},     {"gui", "贵 归"},
        {"guo", "国 过 果"},    {"hai", "还 海"},       {"han", "汉 寒"},
        {"hang", "行 航"},      {"hao", "好 号 浩"},    {"he", "和 合 河"},
        {"hei", "黑"},          {"hen", "很"},          {"hong", "红"},
        {"hou", "后 候"},       {"hu", "湖 户"},        {"hua", "话 花 画"},
        {"huai", "坏 怀"},      {"huan", "换 欢"},      {"huang", "黄"},
        {"hui", "会 回 汇"},    {"huo", "或 活 火"},    {"ji", "机 记 几 集"},
        {"jia", "家 加"},       {"jian", "见 件 间"},   {"jiang", "讲 将"},
        {"jiao", "教 叫 交"},   {"jie", "接 界 姐"},    {"jin", "今 进 金"},
        {"jing", "京 经 静"},   {"jiu", "九 就 酒"},    {"ju", "句 局 剧"},
        {"jue", "觉 绝"},       {"jun", "军"},          {"kai", "开"},
        {"kan", "看"},          {"kang", "康 抗"},      {"kao", "考"},
        {"ke", "可 课 科"},     {"kong", "空 控"},      {"ku", "库"},
        {"kuai", "快"},         {"kuan", "宽"},         {"la", "拉 啦"},
        {"lai", "来"},          {"lan", "蓝 懒"},       {"lao", "老"},
        {"le", "了 乐"},        {"lei", "类 累"},       {"leng", "冷"},
        {"li", "里 理 力 历"},  {"lian", "连 脸"},      {"liang", "两 量"},
        {"liao", "聊 了"},      {"lie", "列"},          {"lin", "林"},
        {"ling", "零 领"},      {"liu", "六 流 留"},    {"long", "龙"},
        {"lou", "楼"},          {"lu", "路 录 陆"},     {"lun", "论 轮"},
        {"luo", "落 罗"},       {"lv", "旅 绿 律"},     {"ma", "吗 妈 马"},
        {"mai", "买 卖"},       {"man", "漫 满 慢"},    {"mang", "忙"},
        {"mao", "猫 毛"},       {"me", "么"},           {"mei", "美 没 每"},
        {"men", "门 们"},       {"meng", "梦"},         {"mi", "米 密"},
        {"mian", "面 免"},      {"ming", "名 明"},      {"mo", "摸 模 末"},
        {"mu", "目 木 母"},     {"na", "那 拿 哪"},     {"nan", "男 南 难"},
        {"ne", "呢"},           {"neng", "能"},         {"ni", "你 尼 泥"},
        {"nian", "年 念"},      {"niao", "鸟"},         {"nin", "您"},
        {"niu", "牛"},          {"nong", "农"},         {"nv", "女"},
        {"pai", "排 拍 牌"},    {"pan", "盘"},          {"pei", "配"},
        {"peng", "朋 碰"},      {"pi", "批 皮"},        {"pian", "片 篇"},
        {"piao", "票"},         {"pin", "频 品 拼"},    {"ping", "平 评"},
        {"pu", "普"},           {"qi", "起 七 期 气"},  {"qian", "前 钱 千"},
        {"qiang", "强"},        {"qiao", "桥"},         {"qin", "亲 琴"},
        {"qing", "请 情 清"},   {"qiu", "球 求 秋"},    {"qu", "去 区 曲"},
        {"quan", "全 权"},      {"que", "却 确"},       {"qun", "群"},
        {"ran", "然"},          {"rang", "让"},         {"re", "热"},
        {"ren", "人 认 任"},    {"reng", "仍"},         {"ri", "日"},
        {"rong", "容"},         {"rou", "肉"},          {"ru", "如 入"},
        {"ruo", "若 弱"},       {"san", "三"},          {"se", "色"},
        {"sen", "森"},          {"sha", "沙 杀"},       {"shan", "山"},
        {"shang", "上 商"},     {"shao", "少 烧"},      {"she", "设 社"},
        {"shen", "身 深 什"},   {"sheng", "生 声 省"},  {"shi", "是 时 事 视 十"},
        {"shou", "手 收 受"},   {"shu", "书 数 树"},    {"shui", "水 谁"},
        {"shuo", "说"},         {"si", "四 思 死"},     {"song", "送 松"},
        {"su", "素 速"},        {"suan", "算"},         {"sui", "岁 随"},
        {"suo", "所 索"},       {"ta", "他 她 它"},     {"tai", "太 台"},
        {"tan", "谈 探"},       {"tang", "汤 糖"},      {"te", "特"},
        {"ti", "题 体 提"},     {"tian", "天 田"},      {"tiao", "条 跳"},
        {"tie", "铁"},          {"ting", "听 停"},      {"tong", "同 通"},
        {"tou", "头 投"},       {"tu", "图 土"},        {"tuan", "团"},
        {"tui", "推 退"},       {"wai", "外"},          {"wan", "完 万 晚"},
        {"wang", "网 王 往"},   {"wei", "为 位 未 微"}, {"wen", "问 文 温"},
        {"wo", "我"},           {"wu", "五 无 物 舞"},  {"xi", "西 系 喜 戏"},
        {"xia", "下 夏"},       {"xian", "先 现 线"},   {"xiang", "想 象 乡"},
        {"xiao", "小 笑 校"},   {"xie", "写 些 谢"},    {"xin", "新 心 信"},
        {"xing", "行 星 性"},   {"xiong", "兄"},        {"xiu", "修"},
        {"xu", "需 许"},        {"xuan", "选"},         {"xue", "学 雪"},
        {"ya", "呀 压"},        {"yan", "眼 言 演"},    {"yang", "样 阳"},
        {"yao", "要 药"},       {"ye", "也 夜 业"},     {"yi", "一 以 已 易"},
        {"yin", "音 因 引"},    {"ying", "影 应 英"},   {"yong", "用 永"},
        {"you", "有 又 游 友"}, {"yu", "与 语 雨 鱼"},  {"yuan", "元 员 原"},
        {"yue", "乐 月 越"},    {"yun", "云 运"},       {"zai", "在 再"},
        {"zan", "暂 赞"},       {"zen", "怎"},          {"zeng", "增"},
        {"zhan", "站 战"},      {"zhang", "张 长 章"},  {"zhao", "找 照"},
        {"zhe", "这 着 者"},    {"zhen", "真 针"},      {"zheng", "正 整 政"},
        {"zhi", "只 知 直 制"}, {"zhong", "中 种 重"},  {"zhou", "周 州"},
        {"zhu", "主 住 注 助"}, {"zhuan", "专 转"},     {"zi", "子 自 字"},
        {"zong", "总 综"},      {"zou", "走"},          {"zu", "组 足"},
        {"zui", "最 嘴"},       {"zuo", "做 作 左"},
    };
    return table;
}

inline std::vector<std::string> pinyinCandidates(const std::string& syllable) {
    std::vector<std::string> candidates;
    if (syllable.empty()) return candidates;

    for (const PinyinSyllable& entry : pinyinTable()) {
        if (syllable != entry.syllable) continue;

        for (const char* cursor = entry.hanzi; *cursor;) {
            const char* end = cursor;
            while (*end && *end != ' ') ++end;
            candidates.emplace_back(cursor, end);
            cursor = *end ? end + 1 : end;
        }
        break;
    }
    return candidates;
}

inline bool PinyinComposer::append(char letter) {
    if (letter >= 'A' && letter <= 'Z') letter = (char)(letter - 'A' + 'a');
    if (letter < 'a' || letter > 'z') return false;
    if (this->buffer.size() >= PINYIN_MAX_SYLLABLE) return false;
    this->buffer += letter;
    return true;
}

inline std::string PinyinComposer::commit(std::size_t index) {
    const std::vector<std::string> list = this->candidates();
    const std::string text              = index < list.size() ? list[index] : std::string{};
    this->buffer.clear();
    return text;
}

inline bool PinyinComposer::backspace() {
    if (this->buffer.empty()) return false;
    this->buffer.pop_back();
    return true;
}

// UTF-8 for one USB/Bluetooth keyboard key, used by the in-app fallback when the
// system keyboard cannot start. key is a brls::BrlsKeyboardScancode, whose
// printable values are the character's ASCII code. Empty when the key does not
// produce text (arrows, modifiers, unknown).
inline std::string keyboardKeyUtf8(int key, bool shift) {
    if (key >= 'A' && key <= 'Z') return std::string(1, (char)(shift ? key : key - 'A' + 'a'));
    if (key >= '0' && key <= '9') {
        static const char* shifted = ")!@#$%^&*(";
        return std::string(1, shift ? shifted[key - '0'] : (char)key);
    }

    switch (key) {
        case ' ':
            return " ";
        case '\'':
            return shift ? "\"" : "'";
        case ',':
            return shift ? "<" : ",";
        case '-':
            return shift ? "_" : "-";
        case '.':
            return shift ? ">" : ".";
        case '/':
            return shift ? "?" : "/";
        case ';':
            return shift ? ":" : ";";
        case '=':
            return shift ? "+" : "=";
        case '[':
            return shift ? "{" : "[";
        case '\\':
            return shift ? "|" : "\\";
        case ']':
            return shift ? "}" : "]";
        case '`':
            return shift ? "~" : "`";
        default:
            return {};
    }
}

// PlSharedFontType.
enum class SharedFont : int {
    Standard             = 0,  // Japan, US and Europe
    ChineseSimplified    = 1,
    ExtChineseSimplified = 2,
    ChineseTraditional   = 3,
    Korean               = 4,
    NintendoExt          = 5,  // Nintendo specific symbols
};

// Borealis font names, see borealis/core/font.hpp.
inline constexpr const char* FONT_NAME_CHINESE_SIMPLIFIED     = "zh-Hans";
inline constexpr const char* FONT_NAME_CHINESE_SIMPLIFIED_EXT = "zh-Hans-ext";
inline constexpr const char* FONT_NAME_CHINESE_TRADITIONAL    = "zh-Hant";
inline constexpr const char* FONT_NAME_KOREAN                 = "korean";
inline constexpr const char* FONT_NAME_SWITCH_ICONS           = "switch";
inline constexpr const char* FONT_NAME_MATERIAL_ICONS         = "material";
inline constexpr const char* FONT_NAME_EMOJI                  = "emoji";

enum class FontOrigin : int {
    Shared,     // runtime shared font (pl), never redistributed
    Resources,  // font shipped inside the application resources
    User,       // font the user dropped into the config directory
};

struct FallbackFont {
    const char* name;
    FontOrigin origin;
    SharedFont shared;  // only meaningful for FontOrigin::Shared
};

// The fallback chain from the most to the least likely glyph source. Every base
// font (FONT_REGULAR and FONT_CHINESE_SIMPLIFIED alike) gets this chain: mixed
// Latin + CJK strings are laid out in FONT_REGULAR, so a chain hung off the
// Chinese base font alone still renders tofu.
inline const std::vector<FallbackFont>& sharedFontFallbacks() {
    static const std::vector<FallbackFont> chain = {
        {FONT_NAME_CHINESE_SIMPLIFIED, FontOrigin::Shared, SharedFont::ChineseSimplified},
        {FONT_NAME_CHINESE_SIMPLIFIED_EXT, FontOrigin::Shared, SharedFont::ExtChineseSimplified},
        {FONT_NAME_CHINESE_TRADITIONAL, FontOrigin::Shared, SharedFont::ChineseTraditional},
        {FONT_NAME_KOREAN, FontOrigin::Shared, SharedFont::Korean},
        {FONT_NAME_SWITCH_ICONS, FontOrigin::Shared, SharedFont::NintendoExt},
        {FONT_NAME_MATERIAL_ICONS, FontOrigin::Resources, SharedFont::Standard},
        {FONT_NAME_EMOJI, FontOrigin::User, SharedFont::Standard},
    };
    return chain;
}

}  // namespace wili::switch2
